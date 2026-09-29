# Telemetry and Advisory: the two structs

Every tick, ChargeXcel sends you a **Telemetry** struct and you send back an
**Advisory**. That's the entire contract, for both ways of plugging in:

- An off-board plugin gets them as JSON and form fields over HTTP. See
  [`http-api.md`](http-api.md) for the exact spelling.
- An on-device script gets the telemetry as a Berry map from
  `dlm.telemetry()`, and sends its advisory with `dlm.report()`. See
  [`on-device-scripts.md`](on-device-scripts.md).

The names below are the firmware's. On the wire and in scripts they are
snake_case (`allowedAmps` → `allowed_amps`).

**Nothing in the Advisory is an input to anything ChargeXcel decides.** A
plugin reports whether it is active and, in a few words, what it is doing;
ChargeXcel shows that on its `/dlm` page and dashboard. That is all a plugin
can say, by design: ChargeXcel is a load manager, not a charger. It cannot
ask a car to draw less, and it does not want to be told to cut power — that
is its own decision, made from its own current sensors.

## Telemetry (ChargeXcel → plugin)

What ChargeXcel tells you, every tick.

| Field | Type | Meaning |
|---|---|---|
| `epochSeconds` | uint32 | Wall-clock time, or `0` if ChargeXcel hasn't synced time yet. |
| `timeTrusted` | bool | Whether `epochSeconds` is real. If you're doing time-of-use logic, check this first. |
| `serviceLegAAmps`, `serviceLegBAmps` | float | The two service legs' current draw, in amps. Deliberately not called "L1"/"L2" — an installer can wire either sensor to either leg, so don't assume which is which. A net-zero plugin needs these, not just the headroom. |
| `evseBranchAmps` | float | What the EV charging circuit itself is currently drawing. |
| `ctReadingsValid` | bool | `false` means the three values above are placeholders, not real measurements — a failed read reads as zero, which looks exactly like a genuinely idle service otherwise. |
| **`allowedAmps`** | float | **The number to plan against.** How much the EV branch may draw right now without pushing either service leg past its continuous limit. While solar charging is active it is lowered further, to what the solar surplus can cover (see [Solar charging](#solar-charging)). |
| `allowedAmpsValid` | bool | `false` means "no opinion," not "unlimited" — an uncommissioned unit, or CT readings not currently trustworthy. With `ctReadingsValid` you can tell which: valid readings and no headroom figure means not commissioned. |
| `relayPermitted`, `relayClosed` | bool | Read-only context: is the charging circuit allowed to be live, and is it actually live right now. Useful for explaining to a user "charging stopped, but it wasn't me." |
| `safetyState` | name | ChargeXcel's own state, sent as its name (e.g. `RUNNING`, `SHED_OFF`, `TRIP_LATCHED`, `THERMAL_OFF`). Context only. |
| `serviceRatingAmps`, `evseBreakerRatingAmps` | float | The electrician's commissioning figures. |
| `continuousCapacityAmps` | float | `min(breaker × 80% or 100%, the max charge rate the electrician entered)` — the most the EV branch may ever be asked for on this installation. |
| `topology` | uint8 | `1` = split-phase 120/240, `2` = split-phase 120/208. Both are two-legged; the value only matters for turning amps into watts. |
| `solarInstalled` | bool | With solar, the service sensors cannot tell export from import, so a plugin doing net-zero charging has to reason about the legs differently. |
| `disconnectDelaySeconds` | float | How long ChargeXcel tolerates a sustained near-limit condition (service current above its trip threshold) before shedding load on its own — the electrician's per-installation setting, typically 1-5 minutes. The gentlest of the three shed rules; informational, so a plugin doing time-of-use or net-zero work knows how much runway it has to react on its own first. |
| `severeOverloadDelaySeconds` | float | The one hardcoded delay behind **both** of the two harder overload rules: total service current over its rating, or (should it ever happen) the EV branch over its own commissioned cap. Currently 30 s for either. Not per-installation, and not two different numbers — see below. |
| `safetyAllowedAmps` | float | The load-protection headroom alone, before solar charging lowers it. Equal to `allowedAmps` whenever solar charging is off, outside its hours, or not the tighter limit. |
| `netzeroPhase` | name | What solar charging is doing: `off`, `outside_window`, `resolving`, `tracking`, `waiting` or `probing`. Sent as `netzero`. See [Solar charging](#solar-charging). |
| `netzeroMode` | name | The owner's choice: `min_solar` (never stops, holds at least 6 A) or `solar_only` (stops when the sun can't cover 6 A). Sent as `netzero_mode`. |
| `netzeroLegA`, `netzeroLegB` | name | Which way each service leg is flowing, as far as ChargeXcel can tell: `export`, `import` or `unknown`. Sent as `netzero_leg_a` / `netzero_leg_b`. |

### On the three shed rules and their delays

ChargeXcel has three internal load-shed rules, and none of them acts
instantly — every one requires a sustained breach first, which is exactly
what these two fields expose:

1. Total service current over its trip threshold → `disconnectDelaySeconds`
   (electrician-configured, ~1-5 min).
2. Total service current over its full rating → `severeOverloadDelaySeconds`.
3. The EV branch over its own commissioned cap (`continuousCapacityAmps`) →
   also `severeOverloadDelaySeconds`, the same number as (2).

`severeOverloadDelaySeconds` reports the current figure, so don't hardcode
30 s in your plugin.

The two delay fields are published to off-board plugins only; a script's
`dlm.telemetry()` map doesn't carry them.

## Solar charging

An installation with solar can have solar charging switched on by its
owner, with a daily time window. While it is active, ChargeXcel lowers
`allowedAmps` so the less-loaded service leg sits about 1 A on the export
side of zero: the car uses solar the house would otherwise send to the grid.
It only ever lowers the figure; `safetyAllowedAmps` is always the ceiling.

The service sensors measure current magnitude, not direction, so export and
import read the same. ChargeXcel works the direction out from how each leg's
reading moves when the car's own current changes by 2 A or more: a reading
that falls as the car draws more was export being absorbed; one that rises
with it was import. That is what the phases describe.

| `netzero` | Meaning | What `allowed_amps` is |
|---|---|---|
| `off` | Solar charging is not switched on, or the installation has no solar. | The plain headroom. |
| `outside_window` | Switched on, but outside the owner's hours, or the unit has no trusted clock yet. | The plain headroom. |
| `resolving` | Still working out which way power flows. | A placeholder: the car's current rate held, or the 6 A minimum. Don't act on it; wait for another phase. |
| `tracking` | Direction known; following the sun. | What the sun can cover, less about 1 A of margin. |
| `probing` | Asking for a deliberate test step to learn the direction: about 3 A up or down, or in `solar_only` a restart at 6 A. | The test figure. Apply it: ChargeXcel can't settle until the car's current actually changes. |
| `waiting` | `solar_only` only: not enough sun for 6 A. | `0`. Stop the car. |

Useful when deciding how fast to react:

- **`allowed_amps` below `safety_allowed_amps` means the cut is the sun,
  not load protection.** You may follow it gently, since clouds pass.
  When `allowed_amps` equals `safety_allowed_amps`, treat any cut as
  urgent.
- **The figure updates every 10 s** from a 10 s average, so polling faster
  doesn't make it move faster.
- **Follow the first change of a session promptly.** A car that starts at
  its usual rate can be far above the sun; there is nothing to wait out.
- **In `solar_only`, a car started while `waiting` is treated as a test.**
  If the start shows enough sun, the phase moves on and a figure appears;
  if not, it returns to `waiting` and `0` after about 3 minutes.
- **A home battery or a zero-export inverter hides the surplus.** Solar
  charging then settles at the minimum.

## Advisory (plugin → ChargeXcel)

What you report. Both fields are shown on `/dlm` and the dashboard, and go nowhere else.

| Field | Type | Meaning |
|---|---|---|
| `present` | bool | "I am alive and actively managing." `false` means running but idle. Over HTTP the poll arriving is already the heartbeat, so this mostly distinguishes idle from active. |
| `action` | string ≤ 31 chars | A few words on what you are doing right now — `24 A to Tesla`, `paused: peak tariff`. Printable ASCII; anything else becomes `?`. |
