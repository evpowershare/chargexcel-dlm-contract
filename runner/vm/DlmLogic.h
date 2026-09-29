#pragma once
#include <cstddef>
#include <cstdint>

#include "NetZeroTypes.h"
#include "SafetyTypes.h"

// The Dynamic Load Management (DLM) plugin contract: what ChargeXcel tells an
// outside program about the service, and the two things that program may say
// back.
//
// ChargeXcel is a load manager, not an EVSE. It measures the service, works
// out how much the EV branch may draw, and opens its relay if the service is
// overdrawn. It has no way to ask a car to draw LESS -- every way of asking
// is vendor-specific (OCPP, Tesla's Fleet API, Modbus, ...) -- so it does not
// try. It publishes its headroom; a plugin, running on some other computer,
// reads that number and speaks the vendor protocol to the actual charging
// equipment itself. Nothing the plugin sends back is an input to any decision
// here: the plugin reports whether it is active and, in a few words, what it
// is doing, and that is shown on /dlm and the dashboard. That is all.
//
// Why that is the safe shape and not a weaker one: with nothing to accept,
// there is nothing to clamp, nothing to expire, and no path -- not even a
// clamped one -- from a plugin to the current limit or the relay. A plugin
// that crashes, lies, floods, or goes quiet changes what the /dlm page says
// and nothing else. The load-shedding code never reads anything from this
// file, and a source check in the build keeps it that way.
//
// This header is platform-free on purpose, so it compiles and runs on a
// desktop as well as on the unit. That is why the commissioning figures
// arrive as plain scalars.

// What ChargeXcel publishes to a plugin each tick. Serialized to JSON for the
// HTTP route; treat it as a wire schema and add fields on the end.
struct DlmTelemetry
{
    // Wall clock, or 0 when the clock has not synced. A plugin doing time-of-use
    // tariff work needs this and must check timeTrusted before believing it.
    uint32_t epochSeconds = 0;
    bool timeTrusted = false;

    // The three current sensors, in amps. Legs A/B are deliberately NOT
    // called L1/L2: an electrician may clamp either service sensor onto
    // either leg, so no plugin may assume which is which. A net-zero plugin needs the legs, not just the headroom.
    float serviceLegAAmps = 0.0f;
    float serviceLegBAmps = 0.0f;
    float evseBranchAmps = 0.0f;
    // False when the readings could not be obtained or failed their quality
    // checks, in which case the three values above are placeholders and must
    // not be read as measurements -- zeros from a failed read are otherwise
    // indistinguishable from a genuinely idle service.
    bool ctReadingsValid = false;

    // THE number a plugin plans against: how much the EV branch may draw right
    // now without pushing either service leg past its continuous limit.
    // Never below 0.
    //
    // allowedAmpsValid == false means it could not be computed at all -- an
    // uncommissioned unit, or sensor readings not currently trustworthy. That
    // is "no opinion", NOT "unlimited". With ctReadingsValid also published a
    // plugin can tell the two apart (valid readings + no headroom figure = not
    // commissioned), which is why there is no separate `commissioned` flag.
    float allowedAmps = 0.0f;
    bool allowedAmpsValid = false;

    // The relay's permission and actual position, plus ChargeXcel's safety
    // state, published so a plugin can explain itself to a user ("charging
    // stopped, but it was not me"). Read-only context.
    bool relayPermitted = false;
    bool relayClosed = false;
    SafetyState safetyState = SafetyState::BOOT_INHIBITED;

    // Commissioning figures, so a plugin knows what envelope it is working
    // inside. continuousCapacityAmps is the smaller of the branch breaker's
    // continuous rating and the maximum charge rate the electrician entered:
    // the most the EV branch may ever be asked for on this installation.
    float serviceRatingAmps = 0.0f;
    float evseBreakerRatingAmps = 0.0f;
    float continuousCapacityAmps = 0.0f;
    // 1 = split phase 120/240, 2 = split phase 120/208 (both two-legged; the
    // value only changes the voltage a plugin would use to turn amps into
    // watts).
    uint8_t topology = 0;
    // Whether the installation has solar. With solar, the service sensors cannot
    // tell export from import, so a plugin doing net-zero charging has to
    // reason about the legs differently -- and ChargeXcel itself publishes
    // no house figure it cannot stand behind.
    bool solarInstalled = false;

    // How long service current held above its trip threshold is tolerated
    // before the relay opens (set by the electrician, 1-5 min per
    // installation). This is the
    // gentlest of the three shed rules -- a plugin doing time-of-use or
    // net-zero work has this long to react to a sustained near-limit
    // condition before ChargeXcel acts on its own.
    float disconnectDelaySeconds = 0.0f;
    // How long BOTH max-limit rules tolerate a breach before shedding: total
    // service current over its rating, and the EV branch over its own cap
    // (which should never happen but is handled the same way if it does).
    // Neither is immediate; both share this one fixed margin, which is not a
    // per-installation setting.
    float severeOverloadDelaySeconds = 0.0f;

    // Net-zero (solar) charging (added fields; schema unchanged). When the owner has
    // it on and it is inside their hours, allowedAmps above is already
    // LOWERED to what the car should draw to use surplus solar -- a plugin
    // that only reads allowedAmps is solar-aware with no change.
    // safetyAllowedAmps is the figure before that, the pure load-shedding
    // headroom, so a plugin can tell "lowered for the sun" (react slowly;
    // clouds pass) from "lowered for safety" (react now).
    float safetyAllowedAmps = 0.0f;
    NetZero::Phase netzeroPhase = NetZero::Phase::Off;
    NetZero::Mode netzeroMode = NetZero::Mode::MinPlusSolar;
    NetZero::LegSign netzeroLegA = NetZero::LegSign::Unknown;
    NetZero::LegSign netzeroLegB = NetZero::LegSign::Unknown;
    // True when allowedAmps is the net-zero figure rather than the plain
    // headroom (it may still equal it when safety is the tighter limit).
    bool netzeroActive = false;
};

// What a plugin says back. Neither field is an input to anything; both are
// shown on /dlm and the dashboard and nowhere else.
constexpr size_t DLM_ACTION_MAX = 31;
struct DlmAdvisory
{
    // "I am alive and actively managing." On the HTTP path the poll arriving
    // is already the heartbeat, so this mostly distinguishes a plugin that is
    // running but idle from one that is doing something.
    bool present = false;
    // A few words on what the plugin is doing right now -- "24 A to Tesla",
    // "paused: peak tariff". Free text, for a person to read.
    char action[DLM_ACTION_MAX + 1] = {};
};

// Per-provider live bookkeeping, RAM only, reset on every boot. The logic
// here treats it as a pure value: state in, new state out.
struct DlmProviderState
{
    // Monotonic time of the last submission of any kind, for the rate limit
    // and for the "last seen" column on the /dlm page. 0 = none this boot.
    int64_t lastSubmissionUs = 0;
    // What the last accepted submission said.
    bool present = false;
    char action[DLM_ACTION_MAX + 1] = {};
    // Lifetime counters for the /dlm page.
    uint32_t recordedCount = 0;
    uint32_t tooFrequentCount = 0;
};

enum class DlmVerdict : uint8_t
{
    // Stored; the /dlm page now shows it.
    Recorded = 0,
    // Arrived faster than MIN_SUBMISSION_INTERVAL_US after the previous one.
    // Nothing is stored -- the previous submission stands.
    TooFrequent,
};

struct DlmDecision
{
    DlmVerdict verdict = DlmVerdict::TooFrequent;
    // The provider state after this submission. The caller stores this back.
    DlmProviderState providerState = {};
};

class DlmLogic
{
public:
    // Fastest accepted submission rate (5 Hz). The intended cadence is 1 Hz;
    // this bounds a runaway or looping plugin's cost to the web server, which
    // is the only thing a plugin can cost this firmware.
    static constexpr int64_t MIN_SUBMISSION_INTERVAL_US = 200'000;

    // A provider that has not submitted for this long is shown as offline.
    static constexpr int64_t OFFLINE_AFTER_US = 30'000'000;

    // Every submission from every provider passes through here. Pure: no
    // clock, no lock, no I/O. Copies the advisory into the state (with the
    // action text sanitised) unless the rate limit refuses it.
    [[nodiscard]] static DlmDecision record(
        const DlmAdvisory& advisory, const DlmProviderState& providerState, int64_t nowUs);

    // Copies `in` to `out` (capacity >= DLM_ACTION_MAX + 1), truncating to
    // DLM_ACTION_MAX and replacing every byte outside printable ASCII with
    // '?'. The page still HTML-escapes it on the way out; this is so the
    // stored copy is plain text a log line or a JSON body can carry safely.
    static void sanitiseAction(const char* in, char* out, size_t capacity);

    // Whether a provider counts as online right now.
    [[nodiscard]] static bool online(const DlmProviderState& state, int64_t nowUs);
};

inline const char* toString(DlmVerdict verdict)
{
    switch (verdict)
    {
        case DlmVerdict::Recorded: return "recorded";
        case DlmVerdict::TooFrequent: return "too_frequent";
    }
    return "unknown";
}
