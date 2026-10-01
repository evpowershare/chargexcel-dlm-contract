# On-device scripts

A ChargeXcel unit can run one small script of your own, written in
[Berry](https://berry-lang.github.io/) (a small language used by Tasmota
that reads a lot like Python). The script runs **on the unit itself**, so it
needs no other computer. On a timer, it reads ChargeXcel's headroom and makes
HTTPS calls to a cloud API, for example your charging station vendor's
or your car maker's, to tell that equipment what to draw.

The one-sentence version, which nothing below changes: **the script reads
ChargeXcel's headroom and talks to a cloud API. It sends nothing back that
ChargeXcel acts on.** What it hands back is only shown on the `/dlm` page.

The examples in [`../scripts/`](../scripts/) are real. `epiccharging.be` and
`smartcar.be` have both run against those vendors' real APIs. Before you
paste a script into a unit, test it on your own computer with
[`../runner/`](../runner/).

## Turning it on

On the unit's web portal, open **`/dlm`** (Load Management Plugins):

1. **What controls the car?**: choose *My own script* and press **Switch**. Only
   one optional connector runs at a time (none, on-device script, or the
   built-in Tesla connector), because each one's memory is set aside once
   when the unit starts. Switching restarts ChargeXcel for about 10 seconds.
   The relay opens and re-closes on its normal schedule.
2. **On-device script**: paste the script and press **Install and run**.
   It compiles on the unit and ticks straight away. The panel shows when it
   last ran, the last error (with a line number) and its last log line;
   memory figures are under *Technical details*.
3. **Secrets**: enter any API keys or account names the script needs.
   The script reads them with `dlm.secret("name")`. Values are never shown
   again. Never put a secret in the script text.

You can keep a few scripts on the unit under names with **Save as** and
switch between them from the *Saved scripts* list.

Off-board plugins (see [`http-api.md`](http-api.md)) keep working whichever
connector is selected.

## The shape of a script

```berry
interval = 60          # seconds between ticks (30..3600, default 60)
import json            # the modules that exist: json, string, math

def tick()
  var t = dlm.telemetry()
  # ... decide, call dlm.http(), ...
  dlm.report(true, "what I did")
end
```

- The whole file runs once when it is installed, and again after an error
  reload. That is where `interval`, the imports and any globals get set.
- `tick()` is then called every `interval` seconds. It must exist.
- Globals survive between ticks, so a script can cache a token or remember
  what it last told the car. They are lost when the script is reinstalled
  or when an error forces a reload.
- `interval` is read once, after the top-level code runs. Missing or not
  an integer means 60. It is clamped to 30..3600.

## What ChargeXcel gives the script

### `dlm.telemetry()` → map

A fresh snapshot each call, with the same figures `/api/dlm/v1/tick` gives an
off-board plugin. Berry integers here are 32-bit and reals are single
precision.

| Key | Type | Meaning |
|---|---|---|
| `epoch` | int | Wall-clock seconds since 1970, or `0` until the unit has synced its clock. |
| `time_utc` | string | The same instant as ISO 8601 UTC, e.g. `2026-10-01T00:20:23Z`, ready for protocols that want a timestamp. |
| `time_trusted` | bool | Whether `epoch` is real. Check it before any time-of-day logic. |
| `ct_valid` | bool | Whether the three current readings are real measurements. `false` means they are placeholders. A failed read's zeros look just like an idle house, so don't read them as amps. |
| `service_leg_a_amps`, `service_leg_b_amps` | real | The two service legs, in amps. They are deliberately not called L1/L2: an electrician may clamp either sensor onto either leg. |
| `evse_branch_amps` | real | What the charging-station branch is drawing now. |
| **`allowed_amps`** | real | **The number to plan against.** How much the charging-station branch may draw right now without pushing either service leg past its continuous limit. Never below 0. While solar charging is active it is lowered further, to what the solar surplus can cover. |
| `allowed_amps_valid` | bool | `false` means "no opinion", **never** "unlimited". Either the unit is not commissioned or its sensors aren't trusted right now. When `false`, `allowed_amps` is 0 and meaningless. |
| `relay_permitted` | bool | Whether ChargeXcel currently allows the charging circuit to be live. |
| `relay_closed` | bool | Whether the circuit actually is live. With `relay_permitted`, a script can say "charging stopped, and it wasn't me". |
| `safety_state` | string | ChargeXcel's own state, e.g. `RUNNING`, `SHED_OFF`, `TRIP_LATCHED`, `THERMAL_OFF`. Context only; `relay_permitted` is the one to branch on. |
| `service_rating_amps` | real | The service rating the electrician entered. |
| `evse_breaker_rating_amps` | real | The charging-station branch breaker. |
| `continuous_capacity_amps` | real | The most the charging-station branch may ever be asked for on this installation. Never plan above it. |
| `topology` | int | `1` = split phase 120/240 V, `2` = split phase 120/208 V. Use it to turn amps into watts: `volts = topology == 2 ? 208 : 240`. (The HTTP route sends this as a string; the script gets the integer.) |
| `solar_installed` | bool | With solar, the service sensors cannot tell export from import, so net-zero logic has to reason about the legs differently. |
| `safety_allowed_amps` | real | The load-protection headroom before solar charging lowers it. When `allowed_amps` is below this, the cut is the sun, not load protection. |
| `netzero` | string | What solar charging is doing: `off`, `outside_window`, `resolving`, `tracking`, `waiting` or `probing`. See [Solar charging](telemetry-and-advisory.md#solar-charging). |
| `netzero_mode` | string | `min_solar` or `solar_only`. |
| `netzero_leg_a`, `netzero_leg_b` | string | `export`, `import` or `unknown`: which way each leg is flowing, as far as ChargeXcel can tell. |

Changing this map changes nothing outside the script.

### `dlm.secret(name)` → string or nil

A value from the script panel's *Secrets* section: up to four secrets, names up to 15
characters, values up to 127. Returns `nil` if that name isn't set. Never
`dlm.log()` a secret.

### `dlm.http(method, url, headers, body)` → `[status, body]`

One HTTP or HTTPS request. The firmware makes the request for the script;
the script never gets a socket.

| Argument | |
|---|---|
| `method` | `"GET"`, `"POST"`, `"PATCH"`, `"PUT"` or `"DELETE"`. |
| `url` | Must start with `http://` or `https://`. Redirects are **not** followed. |
| `headers` | A map of `"Name": "value"` strings, or `nil`. Together they must fit in 1,024 bytes as `Name: value` lines. That is room for a real OAuth bearer token (~830 bytes) plus two or three more headers. |
| `body` | A string, or `nil`. It is sent as given, so set `Content-Type` yourself. |

| `status` | Meaning |
|---|---|
| `100`..`599` | The HTTP status the server sent. |
| `-1` | Transport failure: the unit couldn't connect, it timed out (10 s), or TLS failed. A server whose certificate authority isn't in the unit's trust store also looks like this. |
| `-2` | TLS busy: the unit's one HTTPS session was held by its own firmware-update check for more than 15 s. Try again next tick. |
| `-3` | This tick has already made 16 requests. |
| `-4` | Bad arguments (see above). |
| `-5` | Blocked: the owner has turned on the script's allowed-hosts list on `/dlm`, and this URL's host isn't on it. Nothing was sent. |

`body` is the response body, **cut at 4,096 bytes**. If a list endpoint can
return more than that, use its filter or paging options. It is `""` on any
negative status.

Every call is at least one second after the last one (the firmware waits),
a tick can make at most 16, and the unit has only one HTTPS session at a
time. Time spent waiting inside `dlm.http()` does not count against the
tick's instruction budget.

### The WebSocket door: `on_ws()`, `dlm.ws_send()`, `dlm.ws_peers()`

*New in the firmware release after 0.9.34, as is `time_utc`. The runner in
this repo already has both.*

Some equipment connects *to* its controller instead of being called: an
OCPP charging station opens a WebSocket to its central system and waits to
be told what to do. For that, the unit has one WebSocket door:

```
ws://<unit>/ocpp/<id>
```

- `<id>` is the last path segment (letters, digits, `-`, `_`, `.`; up to 31
  characters). OCPP stations append their charge point id themselves, so
  configure them with `ws://<unit>/ocpp/`.
- **HTTP Basic auth is required.** The password must equal the secret
  `ws_password`; the user name is ignored (OCPP security profile 1). While
  that secret is unset, every connection is refused with `401`.
- **One peer at a time.** A second connection gets `503`. So does any
  connection while no script is installed.
- If the peer offers the `ocpp1.6` subprotocol, it is accepted.
- The connection is plain `ws://`, not `wss://`: the unit can afford only
  one TLS session, and its firmware updates need it. Keep the station on
  the same local network.

The firmware only carries text frames; what they mean is up to the script.

**`on_ws(peer, kind, text)`**: define this and the unit calls it as soon as
something happens on the door, without waiting for the next tick. `kind` is
`"open"`, `"text"` or `"close"`, and `text` is the message for `"text"`,
`nil` otherwise. It runs under the same instruction budget as a tick. If it
calls `dlm.report()`, that report replaces the last one; if not, the last
one stands. An error in `on_ws()` is treated like an error in a tick. A
freshly loaded script gets an `"open"` for a peer that was already
connected.

**`dlm.ws_send(peer, text)` → bool**: sends one text frame to the connected
peer, and returns `true` once it is on the wire. It returns `false` if that
peer isn't connected, the text is empty or over 2,048 bytes, or this tick or
`on_ws()` call has already sent 8 frames.

**`dlm.ws_peers()` → list**: the ids of the connected peers (zero or one).

Messages from the peer are limited to 2,048 bytes as well. A larger
message, a binary frame or a fragmented message closes the connection, and
the peer reconnects.

### `dlm.log(text)`

Writes up to 120 characters to the unit's log and to the *Last log* line on
`/dlm`. Berry's `print()` goes to the same place.

## What the script gives back

### `dlm.report(present, action)`

This is the **only** way back, and it is only displayed:

- `present` (bool): `true` if the script is actively doing something (shown
  as *active*), `false` if it has nothing to do right now (*idle*).
- `action` (string): up to **31 characters** saying what it did. Longer text
  is cut, and anything outside printable ASCII becomes `?`.

If `tick()` returns without reporting, the row shows *idle*. If `tick()`
raises an error, the row shows `error: …`. If a tick reports more than once,
the last report wins.

**Nothing reads `present` or `action` except the web pages.** The script
cannot raise or lower ChargeXcel's limits, open or close the relay, or
change its state. See [`safety-properties.md`](safety-properties.md).

## Limits

- **Modules.** Only `json`, `string` and `math`. `os`, `sys`, `debug`,
  `introspect`, `gc`, `time`, `file` and the rest are compiled out, so
  `import os` is an error. There is no file system, no socket and no
  clock; use `epoch` or `time_utc` for the time. The WebSocket door above
  is owned by the unit; the script only sees its messages.
- **Instruction budget.** A tick gets about 2.1 million Berry instructions.
  Past that it is stopped with `timeout_error`. Waiting on `dlm.http()`
  doesn't count.
- **Memory.** Everything the script allocates (strings, maps, its compiled
  code) comes from a fixed 20 KB arena, never the unit's own memory. When
  the arena runs out, the tick fails with "out of memory". A 4 KB response
  body plus its `json.load()` result is the usual peak. `/dlm` shows the
  peak so you can see your margin.
- **Size.** Up to 8,192 bytes of source, and up to 4,096 bytes per string.
- **Errors.** Any error in a tick (timeout, out of memory, an exception, a
  bad `json.load()`) fails that tick. The unit then throws the VM away and
  rebuilds the script from source after about five minutes.
- **One script per unit.** It runs on its own low-priority task, so a slow
  request stalls only the script, never ChargeXcel.

## Writing a good one

**Only tell the equipment when the answer changes.** Opening an HTTPS session
is the most expensive thing a script does, both for the unit and for the
vendor's API. `smartcar.be` shows the pattern. Every tick it reads the
headroom and reports, but it sends a start or stop only when the decision
changes, plus once every 15 minutes in case a command was lost.
Commanding on every tick means 60 TLS handshakes an hour for a state that
changes a few times a day.

**Expect on/off, not a current knob.** Most cloud EV APIs give third-party
apps start, stop and a state-of-charge limit, not a live amperage setting.
So the usual policy is binary: charge while `relay_permitted` and
`allowed_amps` is at least the car's minimum (6 A), otherwise stop.
`epiccharging.be` is the exception: that vendor accepts a continuous kW
setpoint, so the script splits the headroom across the ports that are
charging and clamps each port to its own maximum.

**Check `allowed_amps_valid` first** and report `false` when there's no
figure. A script that treats "no figure" as "no limit" is wrong.

**Keep `action` short and true.** It is all the owner sees about your
script without opening the log.

## The examples

| Script | Vendor | Policy | Secrets |
|---|---|---|---|
| [`minimal.be`](../scripts/minimal.be) | a made-up API | kW limit every tick; the smallest complete script | `api_key` |
| [`epiccharging.be`](../scripts/epiccharging.be) | epiccharging.com | split the headroom in kW across charging ports | `tenant`, `api_key` |
| [`smartcar.be`](../scripts/smartcar.be) | smartcar.com | start/stop on 6 A, send only on change | `client_id`, `client_secret`, `vehicle_id`, `user_id` |
| [`ocpp.be`](../scripts/ocpp.be) | any OCPP 1.6J charging station | the station connects to the unit; a charging profile in amps (or watts, if the station only takes watts) that lowers at once, raises after 2 min, pauses under 6 A | `ws_password` |

Tesla isn't a script. It is a built-in connector in the firmware.
