# cxl-run: test a script on your computer

`cxl-run` runs a ChargeXcel on-device script with the **same Berry VM and
the same `dlm` module the unit runs**. The files in [`vm/`](vm/) are
byte-for-byte copies of the firmware's; `vm/FIRMWARE_VERSION` says which
release. Only the surroundings are different:

- **Telemetry** is a healthy 100 A service with 24 A of headroom unless
  you change it.
- **Secrets** come from the command line or a file.
- **HTTP** is either canned answers from a file (`--responses`) or real
  requests (`--live`).
- **Time** is simulated: each tick moves `epoch` forward by the script's
  `interval`, so a script that runs hourly doesn't take hours to test.

## Build

Needs a C/C++ compiler and libcurl. macOS has both. On Debian or Ubuntu:
`sudo apt install build-essential libcurl4-openssl-dev`.

```sh
make            # builds ./cxl-run
./tests/run.sh  # every example script, plus the limit checks
```

## Use

```
cxl-run script.be [--ticks N] [--set field=value ...] [--telemetry FILE]
                  [--secret name=value ...] [--secrets FILE]
                  [--responses FILE | --live] [-v]
                  [--ws-listen PORT [--tick-every SECONDS]] [--timeline FILE]
```

- `--set allowed_amps=3`: override one telemetry field. The names are the
  keys `dlm.telemetry()` returns, e.g. `allowed_amps_valid=false`,
  `relay_permitted=false`, `topology=2`, `safety_state=SHED_OFF`.
- `--telemetry FILE`: the same, as `field=value` lines.
- `--secret name=value` / `--secrets FILE`: what `dlm.secret()` returns.
  The unit's limits apply: 4 secrets, names up to 15 characters, values up
  to 127. Keep real keys out of the repo; files ending `.local` are
  git-ignored.
- `--responses FILE`: canned HTTP answers (format below). A request with
  no matching answer gets `-1`, the same as a network failure on the unit.
- `--live`: make the requests for real, through libcurl, at most one a
  second, with a 10 s timeout and no redirects, like the unit.
- `-v`: also print each request body and response body.
- `--ws-listen PORT`: open the unit's WebSocket door on this computer, at
  `ws://<host>:PORT/ocpp/<id>`, with the same rules: Basic auth against the
  `ws_password` secret, one peer, 2,048-byte messages. A real charging station
  or an OCPP simulator can then talk to the script. Ticks run in real time
  until `--ticks` or Ctrl-C, and every frame in or out is printed.
- `--tick-every SECONDS`: with `--ws-listen`, how long a tick really takes
  (default: the script's `interval`). Telemetry time still moves on by
  `interval` each tick, so hold times measured in ticks behave the same,
  only faster.
- `--timeline FILE`: change telemetry at given ticks, one `TICK field=value
  ...` line each. Setting `allowed_amps` also sets `safety_allowed_amps`
  unless the line sets that too. See `scripts/replay/ocpp-timeline.txt`.

### Responses file

```
# comment
> GET https://api.example.com/v1/chargers
< 200
{"chargers": [{"id": 1}]}

> PATCH https://api.example.com/v1/chargers/
< 200
{}
```

A request takes the first unused answer whose method matches and whose URL
starts with the given prefix. Once all the matching answers are used, the
last one repeats.

## Reading the result

Each tick prints its HTTP calls, any `dlm.log()` lines, and what the script
reported (`active`/`idle` plus its text) or its error. At the end it prints
the memory peak.

**About the memory number:** Berry objects take up to twice as much memory
on a 64-bit computer as on the unit's 32-bit chip. The peak here is
therefore an upper bound. If it's under 20,480 bytes (the unit's arena),
the script fits. If it's over, the script *may* still fit, but check the
peak on `/dlm` on a real unit. The runner allows up to 32 KB so a script in
that range still runs.

Exit status: `0` all good, `1` the script failed to load or a tick errored,
`2` it ran but its peak is over 20,480 bytes here.

## What it does *not* check

- **Certificates.** Your computer trusts more certificate authorities than
  the unit does. A vendor whose certificate chain is unusual can pass
  `--live` here and still get `-1` on the unit.
- **The unit's firmware-update check.** On the unit, `dlm.http()` can
  return `-2` for a tick while the firmware-update check holds the one
  HTTPS session. `--live` never does. To test your handling, put `< -2`
  (or `< -1`) as the status in a responses file.
