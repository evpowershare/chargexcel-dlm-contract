// cxl-run: run a ChargeXcel on-device script on your own computer.
//
// This is the same Berry VM and the same `dlm` module the unit runs (vm/ is a
// byte-for-byte copy of the firmware's files). What differs is only the host
// around it: here telemetry and secrets come from the command line, HTTP is
// either canned (--responses) or real (--live, via libcurl), and time is
// simulated so a 60 s interval does not take 60 s.
//
//   cxl-run script.be [--ticks N] [--set field=value ...] [--telemetry FILE]
//                     [--secret name=value ...] [--secrets FILE]
//                     [--responses FILE | --live] [-v]
//                     [--ws-listen PORT [--tick-every SECONDS]] [--timeline FILE]
//
// --ws-listen opens the same WebSocket door the unit has (ws://host:PORT/ocpp/<id>,
// Basic auth against the secret ws_password), so a real charging station or an
// OCPP simulator can connect to the script. Ticks then run in real time, every
// --tick-every seconds (default: the script's interval), until --ticks or Ctrl-C.
// --timeline changes telemetry at given ticks: lines of "TICK field=value ...".
//
// Exit status: 0 all good, 1 the script failed to load or a tick errored,
// 2 it ran but its memory peak is over what the unit guarantees (see below).

#include "DlmScriptVm.h"

#include <curl/curl.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Limits that live in the unit's script service rather than in the VM. The
// VM's own limits (instruction budget, 16 calls per tick, 4 KB response,
// 1 KB of headers, 8 KB of source) come with vm/ and need no copy here.
// ---------------------------------------------------------------------------
namespace unit
{
// The script's memory arena on the unit.
constexpr size_t ARENA_BYTES = 20480;
// Secrets: at most 4, name up to 15 characters, value up to 127.
constexpr size_t SECRET_COUNT = 4;
constexpr size_t SECRET_NAME_MAX = 15;
constexpr size_t SECRET_VALUE_MAX = 127;
// dlm.http(): at least a second between calls, 10 s timeout, no redirects.
constexpr int MIN_HTTP_GAP_MS = 1000;
constexpr long HTTP_TIMEOUT_S = 10;
}

// ---------------------------------------------------------------------------
// The arena. Berry objects are up to twice as big on a 64-bit computer as on
// the unit's 32-bit chip (pointers and values are twice as wide), so the
// peak measured here is an UPPER bound on what the unit will use. Under
// 20 KB here means it fits on the unit. The hard cap here is set higher so
// a script between the two still runs and you can see its number.
// ---------------------------------------------------------------------------
namespace
{
constexpr size_t HOST_ARENA_CAP = 32768;
size_t g_used = 0;
size_t g_peak = 0;

struct Header
{
    size_t size;
    size_t pad;
};
}

extern "C" void* dlm_arena_malloc(size_t size)
{
    if (g_used + size > HOST_ARENA_CAP)
        return nullptr;
    auto* h = static_cast<Header*>(std::malloc(sizeof(Header) + size));
    if (!h)
        return nullptr;
    h->size = size;
    g_used += size;
    if (g_used > g_peak)
        g_peak = g_used;
    return h + 1;
}

extern "C" void dlm_arena_free(void* ptr)
{
    if (!ptr)
        return;
    Header* h = static_cast<Header*>(ptr) - 1;
    g_used -= h->size;
    std::free(h);
}

extern "C" void* dlm_arena_realloc(void* ptr, size_t size)
{
    if (!ptr)
        return dlm_arena_malloc(size);
    if (size == 0)
    {
        dlm_arena_free(ptr);
        return nullptr;
    }
    Header* h = static_cast<Header*>(ptr) - 1;
    if (g_used - h->size + size > HOST_ARENA_CAP)
        return nullptr;
    void* fresh = dlm_arena_malloc(size);
    if (!fresh)
        return nullptr;
    std::memcpy(fresh, ptr, h->size < size ? h->size : size);
    dlm_arena_free(ptr);
    return fresh;
}

extern "C" void dlm_arena_abort(void)
{
    std::fprintf(stderr, "FATAL: Berry called abort()\n");
    std::abort();
}

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------
namespace
{
struct Canned
{
    std::string method, urlPrefix, body;
    int status = 0;
    bool used = false;
};

struct WsPeer
{
    int fd = -1;
    std::string id;
    std::string in;      // bytes received, not yet a whole frame
    std::string partial; // a fragmented message being reassembled
};

struct Runner
{
    DlmTelemetry telemetry = {};
    std::map<int, std::vector<std::pair<std::string, std::string>>> timeline;
    std::vector<WsPeer> peers;
    std::vector<std::string> peerIds; // backing store for hostWsPeers()
    std::map<std::string, std::string> secrets;
    std::vector<Canned> canned;
    bool live = false;
    bool verbose = false;
    std::string body;
    std::chrono::steady_clock::time_point lastHttp = {};
    bool anyHttp = false;
};

Runner g;

std::string trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n");
    const size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool parseBool(const std::string& v)
{
    return v == "1" || v == "true" || v == "yes";
}

bool parseState(const std::string& name, SafetyState& out)
{
    for (int i = 0; i <= static_cast<int>(SafetyState::THERMAL_OFF); ++i)
    {
        const auto s = static_cast<SafetyState>(i);
        if (name == toString(s))
        {
            out = s;
            return true;
        }
    }
    return false;
}

// Field names are the ones the script sees in dlm.telemetry().
bool setTelemetry(const std::string& field, const std::string& value)
{
    DlmTelemetry& t = g.telemetry;
    const float f = std::strtof(value.c_str(), nullptr);
    if (field == "epoch") t.epochSeconds = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
    else if (field == "time_trusted") t.timeTrusted = parseBool(value);
    else if (field == "ct_valid") t.ctReadingsValid = parseBool(value);
    else if (field == "service_leg_a_amps") t.serviceLegAAmps = f;
    else if (field == "service_leg_b_amps") t.serviceLegBAmps = f;
    else if (field == "evse_branch_amps") t.evseBranchAmps = f;
    else if (field == "allowed_amps") t.allowedAmps = f;
    else if (field == "allowed_amps_valid") t.allowedAmpsValid = parseBool(value);
    else if (field == "relay_permitted") t.relayPermitted = parseBool(value);
    else if (field == "relay_closed") t.relayClosed = parseBool(value);
    else if (field == "safety_state") return parseState(value, t.safetyState);
    else if (field == "service_rating_amps") t.serviceRatingAmps = f;
    else if (field == "evse_breaker_rating_amps") t.evseBreakerRatingAmps = f;
    else if (field == "continuous_capacity_amps") t.continuousCapacityAmps = f;
    else if (field == "topology") t.topology = static_cast<uint8_t>(std::atoi(value.c_str()));
    else if (field == "solar_installed") t.solarInstalled = parseBool(value);
    else if (field == "safety_allowed_amps") t.safetyAllowedAmps = f;
    else return false;
    return true;
}

// A healthy, commissioned 100 A service with 24 A of headroom.
void defaultTelemetry()
{
    DlmTelemetry& t = g.telemetry;
    t.epochSeconds = static_cast<uint32_t>(std::time(nullptr));
    t.timeTrusted = true;
    t.ctReadingsValid = true;
    t.serviceLegAAmps = 52.0f;
    t.serviceLegBAmps = 48.0f;
    t.evseBranchAmps = 24.0f;
    t.allowedAmps = 24.0f;
    t.allowedAmpsValid = true;
    t.relayPermitted = true;
    t.relayClosed = true;
    t.safetyState = SafetyState::RUNNING;
    t.serviceRatingAmps = 100.0f;
    t.evseBreakerRatingAmps = 40.0f;
    t.continuousCapacityAmps = 32.0f;
    t.topology = 1;
    t.solarInstalled = false;
    t.safetyAllowedAmps = 24.0f;
}

bool splitPair(const std::string& arg, std::string& k, std::string& v);

// Timeline file: "TICK field=value field=value ...", # comments. Setting
// allowed_amps also sets safety_allowed_amps unless the line names it too
// (no solar cut unless you ask for one).
bool readTimeline(const char* path)
{
    std::ifstream in(path);
    if (!in)
    {
        std::fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    std::string line;
    int n = 0;
    while (std::getline(in, line))
    {
        ++n;
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream ss(line);
        int tick = 0;
        ss >> tick;
        std::string pair, k, v;
        if (tick < 1)
        {
            std::fprintf(stderr, "%s:%d: a line starts with a tick number >= 1\n", path, n);
            return false;
        }
        while (ss >> pair)
        {
            if (!splitPair(pair, k, v))
            {
                std::fprintf(stderr, "%s:%d: cannot use '%s'\n", path, n, pair.c_str());
                return false;
            }
            g.timeline[tick].push_back({k, v});
        }
    }
    return true;
}

void applyTimeline(int tick)
{
    const auto it = g.timeline.find(tick);
    if (it == g.timeline.end())
        return;
    bool safetyNamed = false;
    for (const auto& kv : it->second)
        safetyNamed = safetyNamed || kv.first == "safety_allowed_amps";
    for (const auto& kv : it->second)
    {
        if (!setTelemetry(kv.first, kv.second))
            std::printf("  timeline: unknown field %s\n", kv.first.c_str());
        else
            std::printf("  timeline: %s=%s\n", kv.first.c_str(), kv.second.c_str());
        if (kv.first == "allowed_amps" && !safetyNamed)
            g.telemetry.safetyAllowedAmps = g.telemetry.allowedAmps;
    }
}

bool splitPair(const std::string& arg, std::string& k, std::string& v)
{
    const size_t eq = arg.find('=');
    if (eq == std::string::npos || eq == 0)
        return false;
    k = trim(arg.substr(0, eq));
    v = trim(arg.substr(eq + 1));
    return true;
}

// name=value lines, # comments, blank lines ignored.
bool readPairs(const char* path, bool (*apply)(const std::string&, const std::string&))
{
    std::ifstream in(path);
    if (!in)
    {
        std::fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    std::string line, k, v;
    int n = 0;
    while (std::getline(in, line))
    {
        ++n;
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        if (!splitPair(line, k, v) || !apply(k, v))
        {
            std::fprintf(stderr, "%s:%d: cannot use '%s'\n", path, n, line.c_str());
            return false;
        }
    }
    return true;
}

bool addSecret(const std::string& name, const std::string& value)
{
    if (name.size() > unit::SECRET_NAME_MAX || value.size() > unit::SECRET_VALUE_MAX)
    {
        std::fprintf(stderr, "secret '%s': the unit allows names up to %zu and values up to %zu characters\n",
            name.c_str(), unit::SECRET_NAME_MAX, unit::SECRET_VALUE_MAX);
        return false;
    }
    g.secrets[name] = value;
    if (g.secrets.size() > unit::SECRET_COUNT)
    {
        std::fprintf(stderr, "the unit stores at most %zu secrets\n", unit::SECRET_COUNT);
        return false;
    }
    return true;
}

// Responses file:
//   > METHOD URL-PREFIX
//   < STATUS
//   body lines, up to the next '>' line
// A request takes the first unused block whose method matches and whose
// prefix starts its URL; once all matching blocks are used, the last repeats.
bool readResponses(const char* path)
{
    std::ifstream in(path);
    if (!in)
    {
        std::fprintf(stderr, "cannot read %s\n", path);
        return false;
    }
    std::string line;
    Canned* cur = nullptr;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.rfind("> ", 0) == 0)
        {
            std::istringstream ss(line.substr(2));
            Canned c;
            ss >> c.method >> c.urlPrefix;
            g.canned.push_back(c);
            cur = &g.canned.back();
        }
        else if (cur && cur->status == 0 && line.rfind("< ", 0) == 0)
            cur->status = std::atoi(line.c_str() + 2);
        else if (cur && cur->status != 0)
            cur->body += (cur->body.empty() ? "" : "\n") + line;
        else if (!trim(line).empty() && line[0] != '#')
        {
            std::fprintf(stderr, "%s: unexpected line '%s'\n", path, line.c_str());
            return false;
        }
    }
    for (auto& c : g.canned)
        c.body = trim(c.body);
    return true;
}

// ---------------------------------------------------------------------------
// DlmScriptHost callbacks
// ---------------------------------------------------------------------------
DlmTelemetry hostTelemetry(void*)
{
    return g.telemetry;
}

size_t curlWrite(char* data, size_t size, size_t n, void*)
{
    const size_t len = size * n;
    // The unit keeps the first 4 KB and silently drops the rest.
    const size_t room = DlmScriptVm::HTTP_BODY_MAX - std::min(g.body.size(), DlmScriptVm::HTTP_BODY_MAX);
    g.body.append(data, std::min(len, room));
    return len;
}

int liveHttp(const char* method, const char* url, const char* headers, const char* body)
{
    // Never faster than one call a second, like the unit.
    if (g.anyHttp)
    {
        const auto since = std::chrono::steady_clock::now() - g.lastHttp;
        const auto gap = std::chrono::milliseconds(unit::MIN_HTTP_GAP_MS);
        if (since < gap)
            std::this_thread::sleep_for(gap - since);
    }
    g.anyHttp = true;
    g.lastHttp = std::chrono::steady_clock::now();

    CURL* curl = curl_easy_init();
    if (!curl)
        return DlmScriptVm::HTTP_TRANSPORT_FAILED;
    curl_slist* list = curl_slist_append(nullptr, "Accept-Encoding: identity");
    std::istringstream lines(headers ? headers : "");
    for (std::string h; std::getline(lines, h);)
        if (!h.empty())
            list = curl_slist_append(list, h.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ChargeXcel");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, unit::HTTP_TIMEOUT_S);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWrite);
    if (body)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);

    int status = DlmScriptVm::HTTP_TRANSPORT_FAILED;
    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK)
    {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        status = static_cast<int>(code);
    }
    else
        std::printf("    (transport error: %s)\n", curl_easy_strerror(rc));
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return status;
}

int cannedHttp(const char* method, const char* url)
{
    Canned* pick = nullptr;
    Canned* last = nullptr;
    for (auto& c : g.canned)
    {
        if (c.method != method || std::strncmp(url, c.urlPrefix.c_str(), c.urlPrefix.size()) != 0)
            continue;
        last = &c;
        if (!c.used && !pick)
            pick = &c;
    }
    if (!pick)
        pick = last;
    if (!pick)
    {
        std::printf("    (no canned response for %s %s -- the script sees a transport failure)\n", method, url);
        return DlmScriptVm::HTTP_TRANSPORT_FAILED;
    }
    pick->used = true;
    g.body = pick->body.substr(0, DlmScriptVm::HTTP_BODY_MAX);
    return pick->status;
}

int hostHttp(void*, const char* method, const char* url, const char* headers, const char* body,
    const char** outBody, size_t* outLen)
{
    g.body.clear();
    const int status = g.live ? liveHttp(method, url, headers, body) : cannedHttp(method, url);
    if (status < 0)
        g.body.clear(); // the unit returns an empty body with every local failure
    std::printf("  http %s %s -> %d (%zu bytes)\n", method, url, status, g.body.size());
    if (g.verbose)
    {
        if (body)
            std::printf("    sent: %s\n", body);
        std::printf("    got:  %s\n", g.body.c_str());
    }
    *outBody = g.body.c_str();
    *outLen = g.body.size();
    return status;
}

const char* hostSecret(void*, const char* name)
{
    const auto it = g.secrets.find(name);
    return it == g.secrets.end() ? nullptr : it->second.c_str();
}

void hostLog(void*, const char* text)
{
    std::printf("  log: %s\n", text);
}

void hostYield(void*) {}

// ---------------------------------------------------------------------------
// The WebSocket door (--ws-listen): RFC 6455 text frames, one listening
// socket, at most DlmScriptVm::WS_PEERS_MAX peers -- what the unit offers.
// ---------------------------------------------------------------------------
std::string sha1(const std::string& msg)
{
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::string m = msg;
    const uint64_t bits = static_cast<uint64_t>(msg.size()) * 8;
    m += static_cast<char>(0x80);
    while (m.size() % 64 != 56)
        m += '\0';
    for (int i = 7; i >= 0; --i)
        m += static_cast<char>((bits >> (i * 8)) & 0xff);
    auto rol = [](uint32_t x, int n) { return (x << n) | (x >> (32 - n)); };
    for (size_t off = 0; off < m.size(); off += 64)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(uint8_t(m[off + i * 4])) << 24) | (uint32_t(uint8_t(m[off + i * 4 + 1])) << 16) |
                (uint32_t(uint8_t(m[off + i * 4 + 2])) << 8) | uint32_t(uint8_t(m[off + i * 4 + 3]));
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if (i < 20) f = (b & c) | (~b & d), k = 0x5A827999;
            else if (i < 40) f = b ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
            else f = b ^ c ^ d, k = 0xCA62C1D6;
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e;
    }
    std::string out;
    for (uint32_t x : h)
        for (int i = 3; i >= 0; --i)
            out += static_cast<char>((x >> (i * 8)) & 0xff);
    return out;
}

const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64(const std::string& in)
{
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += B64[v >> 18], out += B64[(v >> 12) & 63], out += B64[(v >> 6) & 63], out += B64[v & 63];
    }
    if (i + 1 == in.size())
    {
        const uint32_t v = uint8_t(in[i]) << 16;
        out += B64[v >> 18], out += B64[(v >> 12) & 63], out += "==";
    }
    else if (i + 2 == in.size())
    {
        const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += B64[v >> 18], out += B64[(v >> 12) & 63], out += B64[(v >> 6) & 63], out += '=';
    }
    return out;
}

std::string unbase64(const std::string& in)
{
    std::string out;
    uint32_t v = 0;
    int n = 0;
    for (char c : in)
    {
        const char* p = std::strchr(B64, c);
        if (!p || !c)
            break;
        v = (v << 6) | static_cast<uint32_t>(p - B64);
        if (++n == 4)
        {
            out += static_cast<char>(v >> 16), out += static_cast<char>((v >> 8) & 0xff), out += static_cast<char>(v & 0xff);
            n = 0, v = 0;
        }
    }
    if (n == 2)
        out += static_cast<char>(v >> 4);
    else if (n == 3)
        out += static_cast<char>(v >> 10), out += static_cast<char>((v >> 2) & 0xff);
    return out;
}

bool sendAll(int fd, const std::string& data)
{
    size_t done = 0;
    while (done < data.size())
    {
        const ssize_t n = ::send(fd, data.data() + done, data.size() - done, 0);
        if (n <= 0)
            return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool sendFrame(int fd, uint8_t opcode, const std::string& payload)
{
    std::string f;
    f += static_cast<char>(0x80 | opcode);
    if (payload.size() < 126)
        f += static_cast<char>(payload.size());
    else
    {
        f += static_cast<char>(126);
        f += static_cast<char>(payload.size() >> 8), f += static_cast<char>(payload.size() & 0xff);
    }
    return sendAll(fd, f + payload);
}

std::string header(const std::string& request, const char* name)
{
    std::istringstream lines(request);
    const size_t len = std::strlen(name);
    for (std::string line; std::getline(lines, line);)
    {
        if (line.size() > len && strncasecmp(line.c_str(), name, len) == 0 && line[len] == ':')
            return trim(line.substr(len + 1));
    }
    return "";
}

// The handshake. Same rules as the unit: path /ocpp/<id>, Basic auth whose
// password is the secret ws_password (refused while it is unset), at most
// WS_PEERS_MAX peers, subprotocol ocpp1.6 echoed when offered.
bool acceptPeer(int fd, std::string& id)
{
    std::string request;
    char buf[1024];
    while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192)
    {
        pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 3000) <= 0)
            return false;
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0)
            return false;
        request.append(buf, static_cast<size_t>(n));
    }
    std::string path;
    {
        std::istringstream first(request);
        std::string method;
        first >> method >> path;
    }
    const std::string key = header(request, "Sec-WebSocket-Key");
    const char* prefix = "/ocpp/";
    id = path.rfind(prefix, 0) == 0 ? path.substr(std::strlen(prefix)) : "";
    const std::string auth = header(request, "Authorization");
    const auto pw = g.secrets.find("ws_password");
    std::string given;
    if (auth.rfind("Basic ", 0) == 0)
    {
        given = unbase64(trim(auth.substr(6)));
        const size_t colon = given.find(':');
        given = colon == std::string::npos ? "" : given.substr(colon + 1);
    }
    const char* refuse = nullptr;
    if (key.empty() || id.empty() || id.size() > DlmScriptVm::WS_PEER_MAX || id.find('/') != std::string::npos)
        refuse = "404 Not Found";
    else if (pw == g.secrets.end() || pw->second.empty() || given != pw->second)
        refuse = "401 Unauthorized";
    else if (g.peers.size() >= DlmScriptVm::WS_PEERS_MAX)
        refuse = "503 Service Unavailable";
    if (refuse)
    {
        std::printf("ws refused %s (%s)\n", path.c_str(), refuse);
        sendAll(fd, std::string("HTTP/1.1 ") + refuse +
                "\r\nWWW-Authenticate: Basic realm=\"ChargeXcel\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        return false;
    }
    std::string reply = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " +
        base64(sha1(key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11")) + "\r\n";
    if (header(request, "Sec-WebSocket-Protocol").find("ocpp1.6") != std::string::npos)
        reply += "Sec-WebSocket-Protocol: ocpp1.6\r\n";
    return sendAll(fd, reply + "\r\n");
}

bool hostWsSend(void*, const char* peer, const char* text, size_t length)
{
    for (const WsPeer& p : g.peers)
    {
        if (p.id == peer)
        {
            std::printf("  ws %s <- %.*s\n", peer, static_cast<int>(length), text);
            return sendFrame(p.fd, 1, std::string(text, length));
        }
    }
    std::printf("  ws %s: not connected, frame dropped\n", peer);
    return false;
}

size_t hostWsPeers(void*, const char** out, size_t capacity)
{
    g.peerIds.clear();
    for (const WsPeer& p : g.peers)
        g.peerIds.push_back(p.id);
    size_t n = 0;
    for (; n < g.peerIds.size() && n < capacity; ++n)
        out[n] = g.peerIds[n].c_str();
    return n;
}

// Pulls whole frames out of p.in. Returns false when the peer must go.
// Text messages land in `texts`.
bool takeFrames(WsPeer& p, std::vector<std::string>& texts)
{
    for (;;)
    {
        if (p.in.size() < 2)
            return true;
        const uint8_t b0 = uint8_t(p.in[0]), b1 = uint8_t(p.in[1]);
        size_t len = b1 & 0x7f, at = 2;
        if (len == 126)
        {
            if (p.in.size() < 4)
                return true;
            len = (uint8_t(p.in[2]) << 8) | uint8_t(p.in[3]);
            at = 4;
        }
        else if (len == 127)
            return false; // nothing here is that big
        const bool masked = b1 & 0x80;
        if (!masked || p.in.size() < at + 4 + len)
            return masked;
        const std::string mask = p.in.substr(at, 4);
        std::string payload = p.in.substr(at + 4, len);
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
        p.in.erase(0, at + 4 + len);
        const uint8_t opcode = b0 & 0x0f;
        if (opcode == 8)
        {
            sendFrame(p.fd, 8, "");
            return false;
        }
        if (opcode == 9)
        {
            sendFrame(p.fd, 10, payload);
            continue;
        }
        if (opcode != 0 && opcode != 1)
            continue; // pong, binary: ignored
        p.partial += payload;
        if (p.partial.size() > DlmScriptVm::WS_FRAME_MAX)
        {
            std::printf("ws %s: message over %zu bytes, dropped\n", p.id.c_str(), DlmScriptVm::WS_FRAME_MAX);
            p.partial.clear();
            continue;
        }
        if (b0 & 0x80)
        {
            texts.push_back(p.partial);
            p.partial.clear();
        }
    }
}

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int)
{
    g_stop = 1;
}

void usage()
{
    std::fprintf(stderr,
        "usage: cxl-run script.be [--ticks N] [--set field=value ...] [--telemetry FILE]\n"
        "                         [--secret name=value ...] [--secrets FILE]\n"
        "                         [--responses FILE | --live] [-v]\n"
        "                         [--ws-listen PORT [--tick-every SECONDS]] [--timeline FILE]\n");
}
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IOLBF, 0); // a log file follows along live
    defaultTelemetry();
    const char* scriptPath = nullptr;
    int ticks = 1;
    bool ticksGiven = false;
    int wsPort = 0;
    double tickEvery = 0;
    std::string k, v;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        const bool hasNext = i + 1 < argc;
        if (a == "--ticks" && hasNext) ticks = std::atoi(argv[++i]), ticksGiven = true;
        else if (a == "--ws-listen" && hasNext) wsPort = std::atoi(argv[++i]);
        else if (a == "--tick-every" && hasNext) tickEvery = std::atof(argv[++i]);
        else if (a == "--timeline" && hasNext) { if (!readTimeline(argv[++i])) return 1; }
        else if (a == "--set" && hasNext)
        {
            if (!splitPair(argv[++i], k, v) || !setTelemetry(k, v))
            {
                std::fprintf(stderr, "unknown telemetry field or value: %s\n", argv[i]);
                return 1;
            }
        }
        else if (a == "--telemetry" && hasNext) { if (!readPairs(argv[++i], setTelemetry)) return 1; }
        else if (a == "--secret" && hasNext)
        {
            if (!splitPair(argv[++i], k, v) || !addSecret(k, v))
                return 1;
        }
        else if (a == "--secrets" && hasNext) { if (!readPairs(argv[++i], addSecret)) return 1; }
        else if (a == "--responses" && hasNext) { if (!readResponses(argv[++i])) return 1; }
        else if (a == "--live") g.live = true;
        else if (a == "-v") g.verbose = true;
        else if (a[0] != '-' && !scriptPath) scriptPath = argv[i];
        else
        {
            usage();
            return 1;
        }
    }
    if (!scriptPath || ticks < 1 || (g.live && !g.canned.empty()))
    {
        usage();
        return 1;
    }

    std::ifstream in(scriptPath, std::ios::binary);
    if (!in)
    {
        std::fprintf(stderr, "cannot read %s\n", scriptPath);
        return 1;
    }
    const std::string source((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (source.empty() || source.size() > DlmScriptVm::SOURCE_MAX)
    {
        std::fprintf(stderr, "%s: the unit accepts 1 to %zu bytes of script, this is %zu\n", scriptPath,
            DlmScriptVm::SOURCE_MAX, source.size());
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    DlmScriptHost host;
    host.telemetry = hostTelemetry;
    host.http = hostHttp;
    host.secret = hostSecret;
    host.log = hostLog;
    host.yield = hostYield;
    host.wsSend = hostWsSend;
    host.wsPeers = hostWsPeers;

    int listenFd = -1;
    if (wsPort > 0)
    {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int one = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(wsPort));
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (listenFd < 0 || ::bind(listenFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listenFd, 4) != 0)
        {
            std::fprintf(stderr, "cannot listen on port %d\n", wsPort);
            return 1;
        }
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        std::signal(SIGPIPE, SIG_IGN);
        if (!ticksGiven)
            ticks = 1 << 30;
        std::printf("ws: listening on ws://0.0.0.0:%d/ocpp/<id>%s\n", wsPort,
            g.secrets.count("ws_password") ? "" : "  (no ws_password secret: every peer is refused)");
    }

    int exitCode = 0;
    size_t worstPeak = 0;
    DlmScriptVm vm;
    char error[DlmScriptVm::ERROR_MAX] = {};

    std::printf("%s: %s HTTP\n", scriptPath, g.live ? "live" : (g.canned.empty() ? "no" : "canned"));
    for (int i = 1; i <= ticks; ++i)
    {
        if (!vm.loaded())
        {
            g_peak = g_used;
            if (!vm.load(source.c_str(), source.size(), host, error, sizeof(error)))
            {
                std::printf("load failed: %s\n", error);
                exitCode = 1;
                break;
            }
            std::printf("loaded: interval %u s, %zu bytes resident\n", vm.intervalSeconds(), g_used);
            worstPeak = std::max(worstPeak, g_peak);
            // Like the unit: a freshly loaded script hears about peers already connected.
            for (const WsPeer& p : g.peers)
                (void)vm.onWebSocket(p.id.c_str(), DlmScriptVm::WsEvent::Open, "", 0);
        }

        if (listenFd >= 0)
        {
            // Real time: serve the door until the next tick is due.
            const double every = tickEvery > 0 ? tickEvery : vm.intervalSeconds();
            const auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<int>(every * 1000));
            while (!g_stop && vm.loaded())
            {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(due - std::chrono::steady_clock::now()).count();
                if (left <= 0 && i > 1)
                    break;
                std::vector<pollfd> fds = {{listenFd, POLLIN, 0}};
                for (const WsPeer& p : g.peers)
                    fds.push_back({p.fd, POLLIN, 0});
                if (poll(fds.data(), fds.size(), i == 1 ? 0 : static_cast<int>(std::max<long long>(left, 0))) < 0)
                    continue;
                std::vector<std::pair<std::string, std::pair<DlmScriptVm::WsEvent, std::string>>> events;
                if (fds[0].revents & POLLIN)
                {
                    const int fd = ::accept(listenFd, nullptr, nullptr);
                    std::string id;
                    if (fd >= 0 && acceptPeer(fd, id))
                    {
                        g.peers.push_back({fd, id, "", ""});
                        std::printf("ws %s connected\n", id.c_str());
                        events.push_back({id, {DlmScriptVm::WsEvent::Open, ""}});
                    }
                    else if (fd >= 0)
                        ::close(fd);
                }
                for (size_t f = 1; f < fds.size(); ++f)
                {
                    if (!(fds[f].revents & (POLLIN | POLLHUP | POLLERR)))
                        continue;
                    auto it = std::find_if(g.peers.begin(), g.peers.end(), [&](const WsPeer& p) { return p.fd == fds[f].fd; });
                    if (it == g.peers.end())
                        continue;
                    char buf[2048];
                    const ssize_t n = ::recv(it->fd, buf, sizeof(buf), 0);
                    std::vector<std::string> texts;
                    bool keep = n > 0;
                    if (keep)
                    {
                        it->in.append(buf, static_cast<size_t>(n));
                        keep = takeFrames(*it, texts);
                    }
                    for (const std::string& t : texts)
                    {
                        std::printf("  ws %s -> %s\n", it->id.c_str(), t.c_str());
                        events.push_back({it->id, {DlmScriptVm::WsEvent::Text, t}});
                    }
                    if (!keep)
                    {
                        std::printf("ws %s disconnected\n", it->id.c_str());
                        events.push_back({it->id, {DlmScriptVm::WsEvent::Close, ""}});
                        ::close(it->fd);
                        g.peers.erase(it);
                    }
                }
                for (const auto& e : events)
                {
                    g_peak = g_used;
                    const DlmScriptVm::TickResult r = vm.onWebSocket(e.first.c_str(), e.second.first,
                        e.second.second.c_str(), e.second.second.size());
                    worstPeak = std::max(worstPeak, g_peak);
                    if (!r.ok)
                    {
                        std::printf("  ERROR in on_ws: %s\n", r.error);
                        vm.unload();
                        exitCode = 1;
                        break;
                    }
                    if (r.reported)
                        std::printf("  report: %s \"%s\"\n", r.advisory.present ? "active" : "idle", r.advisory.action);
                }
                if (i == 1)
                    break;
            }
            if (g_stop)
                break;
            if (!vm.loaded())
                continue;
        }
        applyTimeline(i);
        g_peak = g_used;
        std::printf("tick %d (epoch %u)\n", i, g.telemetry.epochSeconds);
        const DlmScriptVm::TickResult r = vm.tick();
        worstPeak = std::max(worstPeak, g_peak);
        if (r.ok)
            std::printf("  report: %s \"%s\"  [%u http, peak %zu bytes]\n",
                r.advisory.present ? "active" : "idle", r.advisory.action, r.httpCalls, g_peak);
        else
        {
            // The unit shows the error on /dlm, rebuilds the VM and tries again later.
            std::printf("  ERROR: %s  [%u http, peak %zu bytes]\n", r.error, r.httpCalls, g_peak);
            vm.unload();
            exitCode = 1;
        }
        g.telemetry.epochSeconds += vm.loaded() ? vm.intervalSeconds() : DlmScriptVm::INTERVAL_DEFAULT_S;
    }
    vm.unload();
    for (const WsPeer& p : g.peers)
        ::close(p.fd);
    if (listenFd >= 0)
        ::close(listenFd);
    curl_global_cleanup();

    std::printf("memory peak: %zu bytes here; the unit's arena is %zu bytes\n", worstPeak, unit::ARENA_BYTES);
    if (worstPeak > unit::ARENA_BYTES)
    {
        std::printf("  over %zu here does not always mean it won't fit (objects are up to twice as big on\n"
                    "  this computer as on the unit), but it isn't guaranteed. Check /dlm's peak on a unit.\n",
            unit::ARENA_BYTES);
        if (exitCode == 0)
            exitCode = 2;
    }
    if (g_used != 0)
        std::printf("warning: %zu bytes still allocated after unload\n", g_used);
    return exitCode;
}
