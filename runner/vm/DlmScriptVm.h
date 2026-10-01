#pragma once
#include <cstddef>
#include <cstdint>

#include "DlmLogic.h"

// The on-device DLM script runtime: a Berry VM hosting one user-supplied
// script that reads ChargeXcel's headroom and tells the
// charging equipment -- over a cloud API such as epiccharging.com -- what to
// draw.
//
// This class is platform-free on purpose: no FreeRTOS, no ESP-IDF, no
// network. Everything the script can reach outside the VM arrives through
// the DlmScriptHost function table below, so the same VM runs on a desktop
// with the host services substituted. On the unit, one service supplies
// tasks, storage and the HTTP client.
//
// What keeps this safe (each is a source check in the firmware build):
//   - Memory: every VM allocation goes through dlm_arena_* (declared by
//     berry_conf.h), which the unit backs with a fixed arena set aside at
//     boot. The unit's own heap never moves with script behaviour.
//   - Time: the VM heartbeat (every 8,192 instructions) raises a Berry
//     exception past HEARTBEAT_BUDGET, unwinding to be_pcall(). A looping
//     script costs one tick, never the unit.
//   - Reach: the script sees dlm, json, string, math. No os, sys, files,
//     sockets. dlm.http() is the host's own bounded HTTP client, one call at
//     a time, capped body, and the host serialises it against OTA's TLS.
//     dlm.ws_send() answers a WebSocket peer that connected to the host
//     (a charging station speaking OCPP, say); the host owns the socket and
//     hands each incoming frame to the script's on_ws(), so the script still
//     never holds a socket itself.
//   - Decisions: nothing the script says is an input to anything. Its
//     dlm.report() goes to the /dlm page through DlmLogic::record() like any
//     other provider's; its real output goes to the cloud API.

// The host services a script can call, as plain function pointers so a
// test can substitute every one of them.
struct DlmScriptHost
{
    void* context = nullptr;
    // The current telemetry -- the same struct /api/dlm/v1/tick publishes.
    DlmTelemetry (*telemetry)(void* context) = nullptr;
    // One HTTP(S) request. `headers` is "Name: value\n" lines (possibly
    // empty), `body` may be null. Returns the HTTP status, or a negative
    // local failure code (see DlmScriptVm::HTTP_*). On return *outBody points
    // at host-owned memory valid until the next call, of *outLen bytes
    // (<= DlmScriptVm::HTTP_BODY_MAX).
    int (*http)(
        void* context, const char* method, const char* url, const char* headers,
        const char* body, const char** outBody, size_t* outLen) = nullptr;
    // Optional owner allow-list (off by default): false refuses the call
    // before it is made or counted, and dlm.http() answers HTTP_HOST_BLOCKED.
    // Null means no filter. See DlmScriptVm::urlHostAllowed().
    bool (*httpAllowed)(void* context, const char* url) = nullptr;
    // A secret by name (API key, tenant), or null if not set.
    const char* (*secret)(void* context, const char* name) = nullptr;
    // dlm.log() and print() land here.
    void (*log)(void* context, const char* text) = nullptr;
    // Called every YIELD_EVERY_HEARTBEATS heartbeats so a long tick still
    // lets the idle task run. vTaskDelay(1) on the target; no-op on the host.
    void (*yield)(void* context) = nullptr;
    // One text frame to a connected WebSocket peer (<= WS_FRAME_MAX bytes).
    // False if that peer is not connected or the send failed. Null = the
    // host has no WebSocket door, and dlm.ws_send() always answers false.
    bool (*wsSend)(void* context, const char* peer, const char* text, size_t length) = nullptr;
    // The ids of the connected peers, host-owned strings valid until the
    // next call; returns how many were written to `out` (<= capacity).
    size_t (*wsPeers)(void* context, const char** out, size_t capacity) = nullptr;
};

class DlmScriptVm
{
public:
    // Local failure codes dlm.http() returns in place of an HTTP status.
    static constexpr int HTTP_TRANSPORT_FAILED = -1; // connect/timeout/TLS error
    static constexpr int HTTP_TLS_BUSY = -2;          // OTA holds the one TLS session
    static constexpr int HTTP_TOO_MANY = -3;          // MAX_HTTP_PER_TICK exceeded
    static constexpr int HTTP_BAD_ARGS = -4;          // not http(s)://, bad method, etc.
    static constexpr int HTTP_HOST_BLOCKED = -5;      // host not on the owner's allow-list

    // The owner's optional dlm.http() host allow-list: up to 8 entries of
    // "host" or "host:port", matched case-insensitively and exactly (no
    // wildcards, no DNS). An entry without a port matches any port.
    static constexpr size_t HTTP_ALLOW_MAX = 8;
    static constexpr size_t HTTP_ALLOW_HOST_MAX = 63;
    using AllowHost = char[HTTP_ALLOW_HOST_MAX + 1];
    // True when `url`'s host is one of `hosts`. Anything unusual in the
    // authority -- user@ credentials, a backslash, brackets, a percent-escape,
    // a second colon -- is refused outright rather than parsed, since that is
    // exactly where "which host did it really mean" tricks live.
    static bool urlHostAllowed(const char* url, const AllowHost* hosts, size_t count);
    // One line of the owner's list -> a stored entry: trimmed, lower-cased,
    // and a pasted URL reduced to its host[:port]. False for anything that
    // could never match (empty, too long, bad characters).
    static bool normalizeAllowHost(const char* line, size_t length, char* out, size_t capacity);

    static constexpr size_t HTTP_BODY_MAX = 4096;
    // Room for a real OAuth2 bearer token: a typical access token is an
    // ~800-byte JWT, so "Authorization: Bearer <token>\n" alone is ~830
    // bytes. 1024 leaves space for that plus one or two more headers. This
    // buffer is on the script task's stack, not in the arena, so it does not
    // change the script's own memory budget.
    static constexpr size_t HTTP_HEADERS_MAX = 1024;
    // One GET plus up to 15 ports; at >= 1 s per call that is 16 s of a
    // 60 s tick. A site with more ports than that reports "n/N port(s)".
    static constexpr uint32_t MAX_HTTP_PER_TICK = 16;
    // 256 heartbeats x 8,192 instructions = ~2.1 M instructions per tick,
    // well under a second of CPU at 160 MHz. Instruction count, not wall
    // clock: time inside dlm.http() does not eat the budget.
    static constexpr uint32_t HEARTBEAT_BUDGET = 256;
    static constexpr uint32_t YIELD_EVERY_HEARTBEATS = 32;
    // The script's `interval` global, in seconds, clamped. 30 s HTTPS polling
    // fragmented this unit's heap once, so 30 s is the floor.
    static constexpr uint32_t INTERVAL_MIN_S = 30;
    static constexpr uint32_t INTERVAL_MAX_S = 3600;
    static constexpr uint32_t INTERVAL_DEFAULT_S = 60;
    static constexpr size_t SOURCE_MAX = 8192;
    static constexpr size_t ERROR_MAX = 96;
    // WebSocket: the largest text frame either way, the longest peer id (the
    // last path segment the peer connected to), and the most dlm.ws_send()
    // calls one tick or one on_ws() may make.
    static constexpr size_t WS_FRAME_MAX = 2048;
    static constexpr size_t WS_PEER_MAX = 31;
    static constexpr uint32_t WS_SENDS_PER_CALL = 8;
    static constexpr size_t WS_PEERS_MAX = 2;

    struct TickResult
    {
        bool ok = false;
        // What the script said via dlm.report(); present=false if it didn't.
        DlmAdvisory advisory = {};
        char error[ERROR_MAX] = {};
        uint32_t httpCalls = 0;
        uint32_t heartbeats = 0;
        // dlm.report() was called during this call. A tick that never reports
        // reads as present=false; an on_ws() that never reports leaves the
        // last report standing.
        bool reported = false;
    };

    // What happened on a WebSocket, for onWebSocket().
    enum class WsEvent : uint8_t
    {
        Open,  // a peer connected (text is empty)
        Text,  // a text frame arrived
        Close, // the peer went away (text is empty)
    };

    DlmScriptVm() = default;
    ~DlmScriptVm();
    DlmScriptVm(const DlmScriptVm&) = delete;
    DlmScriptVm& operator=(const DlmScriptVm&) = delete;

    // Creates the VM, registers `dlm`, compiles and runs `source`'s top level
    // (under the same budget as a tick), and reads its `interval` global.
    // On failure the VM is torn down, `error` says why, and loaded() is
    // false. Only one DlmScriptVm may be loaded at a time.
    bool load(const char* source, size_t length, const DlmScriptHost& host, char* error, size_t errorCapacity);
    // Calls the script's tick(). Never throws, never leaves anything on the
    // VM stack; on any error the caller should unload() and reload later.
    TickResult tick();
    // Calls the script's on_ws(peer, kind, text) -- kind "open", "text" or
    // "close", text nil unless kind is "text" -- under the same budget as a
    // tick. A script that defines no on_ws() just ignores the event (ok).
    // Same error contract as tick().
    TickResult onWebSocket(const char* peer, WsEvent event, const char* text, size_t length);
    void unload();

    [[nodiscard]] bool loaded() const { return m_vm != nullptr; }
    [[nodiscard]] uint32_t intervalSeconds() const { return m_intervalSeconds; }

    // For the port layer's print() routing (dlm_script_write) only.
    static DlmScriptVm* s_activeForWrite();
    void hostLog(const char* text);

private:
    struct bvm* m_vm = nullptr;
    DlmScriptHost m_host = {};
    uint32_t m_intervalSeconds = INTERVAL_DEFAULT_S;
    // Per-call bookkeeping the native functions and the heartbeat read.
    uint32_t m_heartbeats = 0;
    uint32_t m_httpCalls = 0;
    uint32_t m_wsSends = 0;
    DlmAdvisory m_reported = {};
    bool m_reportedThisTick = false;

    void registerModule();
    bool runProtected(int argc, char* error, size_t errorCapacity);
    void readInterval();
    TickResult finishCall(bool ok, TickResult& result);

    static void obsHook(struct bvm* vm, int event, ...);
    static int nativeTelemetry(struct bvm* vm);
    static int nativeHttp(struct bvm* vm);
    static int nativeReport(struct bvm* vm);
    static int nativeLog(struct bvm* vm);
    static int nativeSecret(struct bvm* vm);
    static int nativeWsSend(struct bvm* vm);
    static int nativeWsPeers(struct bvm* vm);
    static DlmScriptVm* s_active;
};
