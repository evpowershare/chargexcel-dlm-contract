# HTTP API: `POST /api/dlm/v1/tick`

Your plugin runs on any computer — it doesn't need to be anywhere near
ChargeXcel — and polls this one route once a second.

## Step 1 — get a token

Log into ChargeXcel's own web UI (a normal user account, not admin) and
open the **Load Management Plugins** page. Register your plugin with a name;
you'll be shown an `X-DLM-Key` token **exactly once**. Save it immediately —
if you lose it, revoke that entry and register a new one. This step happens
on ChargeXcel's own UI, not through this repo — every ChargeXcel unit
manages its own provider list and tokens locally.

## Step 2 — poll it, once a second

```
POST /api/dlm/v1/tick
X-DLM-Key: <your token>
Content-Type: application/x-www-form-urlencoded

present=1&action=24+A+to+Tesla
```

Both fields are optional — an empty body is a clean "alive, nothing to
report."

| Field | Sent as | Meaning |
|---|---|---|
| `present` | `1` or omitted | Whether you're actively managing right now. |
| `action` | text, ≤ 31 chars | What you're doing, for ChargeXcel's `/dlm` page. |

## Step 3 — read the response

Always JSON on `200`. `401` if your key is missing, wrong, or revoked;
`429` if you polled faster than 5 Hz (the previous report stands).

```json
{
  "schema": 2,
  "telemetry": {
    "epoch": 1794345600, "time_trusted": true,
    "ct_valid": true, "service_leg_a_amps": 22.4, "service_leg_b_amps": 18.1,
    "evse_branch_amps": 16.0,
    "allowed_amps": 24.5, "allowed_amps_valid": true,
    "relay_permitted": true, "relay_closed": true,
    "safety_state": "RUNNING",
    "service_rating_amps": 200.0, "evse_breaker_rating_amps": 50.0,
    "continuous_capacity_amps": 40.0, "topology": "split_phase_120_240",
    "solar_installed": false,
    "disconnect_delay_seconds": 120.0, "severe_overload_delay_seconds": 30.0,
    "safety_allowed_amps": 24.5, "netzero": "off", "netzero_mode": "min_solar",
    "netzero_leg_a": "unknown", "netzero_leg_b": "unknown"
  }
}
```

`telemetry` is the struct described in
[`telemetry-and-advisory.md`](telemetry-and-advisory.md), in snake_case.
`topology` is spelled out as `split_phase_120_240` / `split_phase_120_208`.

`telemetry.allowed_amps` is what you plan around. A minimal plugin loop:
poll, read it, decide what the car should draw, tell the car (or the EVSE)
in whatever protocol it speaks, and report what you did in `action` on the
next poll. ChargeXcel never needs to know which protocol that was.

With solar charging switched on by the owner, `allowed_amps` is already
lowered to what the solar surplus can cover, so a plugin that only reads
`allowed_amps` follows the sun with no changes. `safety_allowed_amps` is the
figure before that: the plain load-protection headroom. The `netzero*`
fields say what solar charging is doing; see
[Solar charging](telemetry-and-advisory.md#solar-charging) for what each
value means and how to react to it.

## Being a good citizen

- **Poll at 1 Hz.** Faster than 5 Hz gets you a `429`. The intended cadence
  is exactly 1 Hz, where the poll doubles as your liveness heartbeat —
  ChargeXcel shows your plugin as offline after 30 s of silence.
- **Keep `action` honest and short.** It is the only thing the owner sees
  about your plugin without reading your logs.

See [`reference-plugin/`](../reference-plugin/) for a complete working loop.
