# web_climate_editor

The JSON/REST API of the `climate_hub` thermostats, served on the device's own web server under
`/climate-editor/api/`. List, read, create, change, start, stop and delete thermostats, edit their
presets, move a target, watch the control loop, and discover the sensors and relays a thermostat
may name — everything a browser editor does, and everything `curl` can do without one. ESP-IDF
only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, climate_hub, loop_job, web_origin_guard, web_climate_editor]

web_server:
  port: 80

littlefs_storage:
  id: user_storage

climate_hub:
  storage: user_storage

web_climate_editor:
```

## Options

| Option           | Default           | Meaning                                                                  |
| ---------------- | ----------------- | ------------------------------------------------------------------------ |
| `climate_hub_id` | the only `climate_hub` | The hub to edit                                                     |
| `url_prefix`     | `/climate-editor` | The routes live at `<url_prefix>/api/`. A path web_server answers itself (`/climate`, `/switch`, `/events`, …), anything overlapping `/api/device`, and a path on or below another component's `url_prefix` are refused |

The handler registers on the shared `web_server_base`, so it answers on the same port as
`web_server` and behind its `auth:` credentials. Those credentials come from the `web_server:`
block and from nowhere else: with no `web_server:`, or no `auth:` in it, anything that can reach
the port can rewrite every thermostat and switch the relays they drive. The configs in this
repository set them, and [`web_auth`](../web_auth/README.md) lets the device replace the pair.
Every route is also behind [`web_origin_guard`](../web_origin_guard/README.md), which answers
`403` to a request whose `Origin` is not the `Host` it was sent to, so a page on another site
cannot use the credentials a browser has cached. Clients that send no `Origin`, `curl` among
them, are unaffected.

## REST

Routes match on the exact URL path under `<url_prefix>/api/`, and each answers the one method
below. A `GET` or `POST` the route does not take, and an `OPTIONS`, is `405` with an `Allow`
header naming the one it does. The server takes no other method to any path: a `PUT`, `DELETE`,
`HEAD` or `PATCH` gets its own `405`, with an HTML sentence for a body, no `Allow` and no JSON,
and the connection closed. Any other path under the prefix is `404`.

Every route that touches the thermostats runs its whole read or write on the loop task, which
owns them, and answers `503 Service Unavailable` when the loop does not get to it within five
seconds. Nothing was read or written then, and the call can simply be made again. `schema` and
`ping` answer from build-time data and never wait for the loop.

| Method | Path | |
|---|---|---|
| GET | `list` | `{"success": true, "count", "max_controllers", "controllers": [{"id", "name", "enabled", "kind", "mode", "sensor_id", "heat_relay_id", "cool_relay_id", "running", "waiting", "active_preset", "active_preset_name"}, ...]}`; `waiting` says why an enabled thermostat does not run, `""` when it runs or is disabled; `active_preset` is the key of the preset picked last and `active_preset_name` its name, both `""` for none |
| GET | `get?id=` | One thermostat, in the file format of [climate_hub](../climate_hub/README.md#a-thermostat), its presets included |
| POST | `save` | A thermostat as a JSON body. `id` absent or `""` creates one, an existing `id` replaces that one, renamed or not. Answers `{"success": true, "message", "id"}`, and a `warning` when it was saved enabled but does not run |
| POST | `delete?id=` | Stops the thermostat and removes its file. `{"success": true, "message", "persisted"}`; `persisted` is `false` when the thermostat is gone but its file is not, so the next boot brings it back |
| POST | `enable?id=&value=true\|false[&take_over=true]` | Starts or stops it and stores the flag. `{"success": true, "message", "persisted"}`; `persisted` is `false` when the change is live but the flag did not reach flash. A `warning` comes as from `save` when it was enabled but does not run |
| POST | `setpoint?id=&value=` | Moves the target, clamped into the thermostat's range, whether it runs or not |
| GET | `status[?id=]` | `{"success": true, "controllers": [...]}`: per thermostat whether it runs and the `waiting` of `list`, what it does (`off`, `idle`, `heating`, `cooling`), its fault, the room temperature and its age, the target, the active preset as `list` gives it, the range, the bang-bang switching points, the heat and cool duty and relay state, and a running PID's terms. A stopped thermostat still reports its sensor's reading and its active preset |
| GET | `entities` | `{"success": true, "sensors": [{"object_id", "name", "unit"}], "switches": [{"object_id", "name", "claimed_by"}]}`; `claimed_by` is the id of the running thermostat that holds the relay, or `""`; an enabled one that waits reserves its relays all the same (`list` shows which). Internal entities are left out, and so is a sensor that does not report °C |
| GET | `schema` | The kinds, modes and faults, `max_controllers`, `name_max_length`, `presets` (`{"max_count": 8, "modes": ["keep", "off", "heat", "cool", "heat_cool"], "standard": ["eco", "away", "boost", "comfort", "home", "sleep", "activity"]}`), and every tunable number with its label, unit, default, range, step and hint, grouped as a form shows them: the table the device clamps against |
| GET | `ping` | `{"status": "ok"}` |

`save` reads a raw JSON body, so it needs a `Content-Type` that is not a form:
`application/json`. A form body (`application/x-www-form-urlencoded`, or no `Content-Type` at
all) is parsed into fields by the server instead: up to 1024 bytes it is answered
`Empty request body`, and a longer one gets the server's own bare `400` before it reaches this
API. A JSON body over 8 KiB is `413`. A missing key, in a nested object too, takes its default
and every number is clamped into its range, so a partial document is accepted: `name`,
`sensor_id` and `heat.relay_id` (or `cool.relay_id` with a `mode` that uses it) are enough.
`presets` left out, or `null`, is none, so a Save without them removes them.

The device gives the presets their keys: a create makes every key from its preset's name, and a
Save keeps the key of every preset that comes back with it and makes one for a new preset. A
form sends back the key it got and none for a new preset. `active_preset` is the thermostat's
state, not the form's: a Save ignores it, keeps the active preset while its key is in the list,
and applies that preset's new target and mode at once.

The refusals come in this order, and the first one a document meets is the answer:

1. `400`: the body is empty or not JSON, or the document breaks a rule of the file format — the
   thermostat's structure first, then its presets, then its name rules (`Name is required`,
   `Name cannot contain '/'`, …). A preset's sentence names its row from 1:
   `Preset 2: "none" is reserved`;
2. `404` for an `id` no thermostat has, `409` for one whose file a newer firmware wrote
   (`A newer firmware wrote this thermostat; update the firmware to change it`), or `507` when a
   create would pass `max_controllers`;
3. `409` for a name another thermostat or a YAML climate answers to, compared without case and
   extra spaces or by the entity id both would get (`Room 1` and `Room_1`), and on a create for
   a name whose every id is taken by a file in the folder;
4. for an enabled thermostat, `400` when its sensor does not report °C
   (`"Uptime" reports s, not °C`), then `409` when a running thermostat holds its relay:
   `"Relay 1" is already driven by "Living Room"`, and then when another enabled thermostat that
   waits names it: `"Relay 1" is reserved by "Attic", which is enabled and waits to start`. A
   relay the thermostat holds already is never refused;
5. `413` when the file the document makes would be over 8 KiB, a guard no document within the
   rules reaches.

An enabled thermostat whose sensor or a relay is not on the device is no refusal: it is saved,
stays enabled and waits without running, and a running one is stopped. The answer is `200` with
a `warning` that names what is missing, and the message repeats it:

```json
{"success": true, "message": "Thermostat created; not started: sensor 'attic' not found",
 "id": "attic-room", "warning": "not started: sensor 'attic' not found"}
```

It starts at the next boot that finds what it names, or at a Save or an `enable` that finds it
there. The same `warning` comes, as `not started: no free climate entity`, if no climate entity
is free. Until it starts or is stopped, `list` and `status` show it enabled, not running and with
no fault, with the latest reason in `waiting`. One the boot left waiting shows there too, and the
boot has two reasons more: a sensor not in °C (`not started: sensor 'uptime' reports s, not °C`)
and a relay another thermostat holds (`not started: relay 'relay_1' is held by 'living-room'`),
which only files written by hand or a restore bring about.

One that waits for a relay starts as soon as the relay is free, the first by id when several
wait for it, and one that still cannot start gets the reason it has now. The answer to the
`enable`, `delete` or `save` that freed the relay names who started, before any `warning` of
its own: `Thermostat disabled; "Winter" started`.

`enable` starts a thermostat as a Save of it would: `400` when its sensor does not report °C,
then `409` when a running thermostat holds its relay or an enabled one that waits names it, and
one whose sensor or a relay is not on the device is stored enabled and waits, with the same
`warning` beside `persisted`, whether it was stored enabled before or not. With
`take_over=true` each of those is stored as disabled first, the running one stopped, in the
same step, and the answer names them, the running one first:
`Thermostat enabled; "Living Room" and "Attic" stopped`. A relay the stopped one drove alone is
then free for whoever waits for it, after this one has started. A take-over by one whose sensor
or a relay is not on the device is `400` instead (`No sensor "attic" on this device`), and
nothing changes; so is a `409` (`No free climate entity to run it in`) when only waiting
thermostats name its relays and every climate entity is taken. With no other enabled thermostat
on its relays, `take_over=true` changes nothing.

An `id` is the thermostat's slug (`a-z`, `0-9`, single dashes, at most 48): a missing one is
`Missing id parameter`, anything else `Invalid id parameter`. `value` and `take_over` of
`enable` are `true` or `false` exactly, and `value` of `setpoint` a plain decimal number; `nan`,
`inf`, hex and padding are refused. A `+` in it goes as `%2B`: the server decodes a bare `+`
to a space.

A thermostat whose file a newer firmware wrote is listed, read and run like any other, and
`enable`, `setpoint` and `delete` act on it, but only `delete` reaches the file: the rest lasts
until the next reboot, and `enable` answers `"persisted": false` when it changed the flag or took
a relay over; the thermostats it stopped, running or waiting, then stay enabled in their files.
Its `version` in `get` is the file's own, higher than this firmware's `2`.

Every failure is `{"success": false, "error"}`, with the sentence an editor shows: `400` for a
bad request, `404` for an unknown `id` or path, `405` for a `GET` or `POST` the route does not
take, `409` for a name or a relay in use or reserved or a thermostat a newer firmware wrote, `413`
for an oversized body or file, `500` when nothing could be written, `503` when the loop was busy
and `507` at `max_controllers`. A `500` changed nothing: `The thermostat's file could not be
written`, or `Thermostat storage is not available` when the storage was not usable at boot —
then every write gets it, a save before its body is even read. The same contract,
machine-readable: [openapi.yaml](openapi.yaml) (OpenAPI 3.1).

A rename reaches Home Assistant as a new entity, and a thermostat that starts, stops, is removed
or renamed makes Home Assistant reconnect: [doc/CLIMATE.md](../../doc/CLIMATE.md).

A firmware with an `auth:` block wants the credentials on every one of these; `--digest -u`
covers what the configs in this repository build.

```sh
A='--digest -u admin:admin'
curl $A 'http://<device>/climate-editor/api/list'
curl -X POST $A 'http://<device>/climate-editor/api/save' -H 'Content-Type: application/json' \
  --data-binary @living-room.json
curl -X POST $A 'http://<device>/climate-editor/api/setpoint?id=living-room&value=21.5' -d ''
curl -X POST $A 'http://<device>/climate-editor/api/enable?id=guest-room&value=true&take_over=true' -d ''
curl $A 'http://<device>/climate-editor/api/status?id=living-room'
```

A POST without a body needs `-d ''`: `curl` then sends the `Content-Length: 0` that
ESP-IDF's server insists on (`411` without it), as a browser's `fetch` does on its own.

## client/

`client/climateApi.ts` is the TypeScript client for these routes, `client/types.ts` the wire
types, `client/naming.ts` the name rules an editor needs to predict the device (which names are
refused, which collide, which id a create gets, which preset names are refused or built in, and
which key a preset gets), and `client/mock/climateMock.ts` a dependency-free in-memory
implementation of the same routes, with a simulated room per sensor, for a dev server or unit
tests; its `control()` sets a running thermostat's mode, target or preset the way Home Assistant
does, and its `seed` option replaces the thermostats it starts with, one from a newer firmware
among them if its `version` says so. They are the contract a browser client codes against and
they live here so they change with the C++ that they mirror. Nothing in this repository builds
or type-checks them yet.

## Testing

`python tests/run.py web_climate_editor` builds the handler for the ESPHome `host` platform
over the real hub, a directory standing in for the flash and the harness's `web_server_base`
stand-in, and drives every route through it; the cases are in
`tests/components/web_climate_editor/`. See [doc/TESTING.md](../../doc/TESTING.md). What the
host cannot reach is ESP-IDF's side of the answer: the status lines written by hand, the hop
from the server's task to the loop task, and the methods the server answers itself.

`tests/components/web_climate_editor/contract.json` lists requests and the answers the device
gives them. The suite sends each one through the handler, and the dashboard runs the same file
against `client/mock`, so a change to an answer goes into the file, the C++ and the mock
together. Its format, and which cases go in it, are in
[doc/TESTING.md](../../doc/TESTING.md#thermostat-editor-cases).
