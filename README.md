# ChargeXcel DLM plugins

DLM = Dynamic Load Management. **ChargeXcel is a load management system, not
an EV charger.** It sits on your electrical service, measures it with its own
current sensors, and sheds load by opening its relay before the service is
overdrawn. The EV charging station or car is a separate device downstream of
it.

<p align="center">
  <img src="docs/images/chargexcel-diagram.gif" width="600"
       alt="Animated diagram: a house with a 100 A electric panel and 60 A of household loads (PC, lights, A/C, washer, oven). ChargeXcel sits between the panel and the charging station, which charges a car at 40 A.">
</p>

In this example the house has a 100 A service and its own appliances are
drawing up to 60 A. ChargeXcel watches the whole service, so the charging station
and everything else in the house together stay within what the service can
carry.

ChargeXcel knows how much spare capacity your service has, second by second.
For Tesla vehicles, it has a built-in connector that sets the charging current
directly through Tesla's cloud service. For everything else, it can't ask a car
to draw *less*; all it can do is cut the circuit. So it publishes that
headroom, and a **plugin** reads it, decides what the car or charging station
should draw, and tells that equipment what to do in its own protocol or cloud
API. All the plugin sends back is a status line for the owner's `/dlm` page:
whether it's active, and a few words on what it is doing.

This repo is everything you need to write one. It is **not** ChargeXcel's
firmware.

## The one rule

**Nothing a plugin sends is an input to any decision ChargeXcel makes.** No
field changes the current limit, the relay, or when load is shed. The firmware
enforces this in both directions, and the check itself is tested by
deliberately breaking it. So you can't break ChargeXcel by accident, and a
compromised plugin can't talk it into anything. ChargeXcel keeps shedding load
from its own sensors whether zero plugins are running or ten. See
[`docs/safety-properties.md`](docs/safety-properties.md).

## Two ways to plug in

| | On-device script | Off-board plugin |
|---|---|---|
| Runs on | ChargeXcel itself | any computer on the network (a Raspberry Pi, a server, a laptop) |
| Language | [Berry](https://berry-lang.github.io/) (a small Python-like language) | anything that can make HTTP requests |
| Talks to the equipment through | HTTPS calls to a cloud API (`dlm.http()`), or an OCPP charging station that connects to the unit itself (`on_ws()` / `dlm.ws_send()`) | whatever you like: OCPP, a local API, a cloud API |
| Good for | vendors with a cloud API; no extra hardware | local protocols, heavy logic, anything the script limits rule out |
| Start with | [`docs/on-device-scripts.md`](docs/on-device-scripts.md), [`scripts/`](scripts/) | [`docs/http-api.md`](docs/http-api.md), [`reference-plugin/`](reference-plugin/) |

A unit runs at most one on-device script. Off-board plugins work alongside
it, and each has its own key. You choose between the on-device script, no
connector, and ChargeXcel's **built-in Tesla connector** under "What controls the car?"
on the unit's `/dlm` page. Tesla support is part of the firmware, not a
plugin.

## What's here

- [`docs/telemetry-and-advisory.md`](docs/telemetry-and-advisory.md): every
  figure ChargeXcel publishes, and the two things you send back.
- [`docs/on-device-scripts.md`](docs/on-device-scripts.md): the script API,
  its limits, and how to write a good script.
- [`docs/http-api.md`](docs/http-api.md): the `/api/dlm/v1/tick` route for
  off-board plugins.
- [`docs/safety-properties.md`](docs/safety-properties.md): what the
  contract guarantees, each stated as the failure it prevents.
- [`scripts/`](scripts/): example on-device scripts. `epiccharging.be` and
  `smartcar.be` have run against those vendors' real APIs. `ocpp.be` is an
  OCPP 1.6J central system: a charging station connects straight to the
  unit, and the script keeps a charging profile on it that follows the
  headroom. It has been run against the
  [MicroOcppSimulator](https://github.com/matth-x/MicroOcppSimulator).
- [`runner/`](runner/): `cxl-run`, which tests a script on your own computer
  with the same Berry VM and limits the unit uses.
- [`reference-plugin/`](reference-plugin/): a complete off-board plugin in
  about 150 lines of Python.
- [`home-assistant/`](home-assistant/): a read-only package that shows
  ChargeXcel's readings, headroom and safety state in Home Assistant. It is
  not a plugin, and it needs no registration on `/dlm`.

## Try a script in two minutes

```sh
make -C runner
./runner/cxl-run scripts/smartcar.be --ticks 3 \
    --responses scripts/replay/smartcar.txt --secrets scripts/replay/smartcar.secrets
./runner/cxl-run scripts/smartcar.be --set allowed_amps=3 \
    --responses scripts/replay/smartcar.txt --secrets scripts/replay/smartcar.secrets
```

The first command starts the car and then stays quiet. The second one sees
only 3 A of headroom and stops it.

### Try the OCPP script with a simulated charging station

> The WebSocket door `ocpp.be` needs on a unit arrives in the firmware
> release after 0.9.34. The runner below already has it.

`cxl-run --ws-listen` opens the same WebSocket door the unit has, so an OCPP
1.6J station or simulator can connect to the script on your computer:

```sh
./runner/cxl-run scripts/ocpp.be --ws-listen 9000 --tick-every 2 \
    --secret ws_password=demo-pass --timeline scripts/replay/ocpp-timeline.txt
```

Point the station at `ws://<your-computer>:9000/ocpp/` with charge point id
`CP1` and authorization key (Basic auth password) `demo-pass`. For
MicroOcppSimulator, which serves its own web UI on port 8000:

```sh
curl -X POST http://127.0.0.1:8000/api/websocket \
    -d '{"backendUrl":"ws://127.0.0.1:9000/ocpp","chargeBoxId":"CP1","authorizationKey":"demo-pass"}'
```

The timeline drops, pauses, resumes and caps the headroom, and the comments
in it say which charging-profile limit each step should produce. On a unit,
paste `ocpp.be` on the `/dlm` page, set the secret `ws_password`, and point
the station at `ws://<unit>/ocpp/`.

## Versioning

The tick response carries `"schema": 2`. A breaking change to what an
off-board plugin receives gets a new number. The script API is versioned
with the firmware. [`runner/vm/FIRMWARE_VERSION`](runner/vm/FIRMWARE_VERSION)
says which firmware release the runner's VM was copied from.

## License

MIT. See [`LICENSE`](LICENSE). The Berry interpreter under `runner/vm/berry/`
is also MIT (© Guan Wenliang); see its own
[`LICENSE`](runner/vm/berry/upstream/LICENSE).
