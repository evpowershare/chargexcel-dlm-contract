#pragma once
#include <cstdint>

// Net-zero (solar) charging: the status values ChargeXcel publishes to a
// plugin alongside its headroom. When net-zero is on and inside the owner's
// hours, the published allowed_amps is already lowered to what the car
// should draw to use surplus solar; these say how that figure was reached.
//
// The service sensors measure current size, not direction, so export and
// import read the same. ChargeXcel works the direction out from how each
// leg's reading moves when the car's own current changes: a reading that
// falls as the car draws more was export being absorbed; one that rises
// with it was import.
namespace NetZero
{
enum class Mode : uint8_t
{
    // Never stop the car for lack of sun: hold at least minAmps from the
    // grid and add whatever solar there is on top (evcc's "Min+Solar").
    MinPlusSolar = 0,
    // Stop when the surplus cannot cover minAmps; restart once it returns.
    SolarOnly = 1,
};

// The direction of one leg's flow at the service, as far as we can tell.
enum class LegSign : uint8_t
{
    Unknown = 0,  // never tested, or lost (reading passed near zero, or an
                  // unexplained jump that could have crossed zero)
    Export,
    Import,
};

enum class Phase : uint8_t
{
    Off = 0,        // not enabled, or no solar on this installation
    OutsideWindow,  // enabled, but outside the owner's hours (or no trusted
                    // clock): normal headroom applies
    Resolving,      // active; at least one leg's direction is not known yet
    Tracking,       // active; both legs known (or sitting at zero)
    Waiting,        // solar only: stopped for lack of surplus
    Probing,        // a deliberate test step is being asked for
};

inline const char* toString(Phase phase)
{
    switch (phase)
    {
        case Phase::Off: return "off";
        case Phase::OutsideWindow: return "outside_window";
        case Phase::Resolving: return "resolving";
        case Phase::Tracking: return "tracking";
        case Phase::Waiting: return "waiting";
        case Phase::Probing: return "probing";
    }
    return "unknown";
}

inline const char* toString(LegSign sign)
{
    switch (sign)
    {
        case LegSign::Unknown: return "unknown";
        case LegSign::Export: return "export";
        case LegSign::Import: return "import";
    }
    return "unknown";
}

inline const char* toString(Mode mode)
{
    return mode == Mode::SolarOnly ? "solar_only" : "min_solar";
}
}  // namespace NetZero
