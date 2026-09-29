# web_climate_editor

The JSON/REST API of the `climate_hub` thermostats, served on the device's own web server under
`/climate-editor/api/`. List, read, create, change, start, stop and delete thermostats, move a
target, watch the control loop, and discover the sensors and relays a thermostat may name —
everything a browser editor does, and everything `curl` can do without one. ESP-IDF only.

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
below: anything else is `405` with an `Allow` header. Any other path under the prefix is `404`.

Every route that touches the thermostats runs its whole read or write on the loop task, which
owns them, and answers `503 Service Unavailable` when the loop does not get to it within five
seconds. Nothing was read or written then, and the call can simply be made again. `schema` and
`ping` answer from build-time data and never wait for the loop.

| Method | Path | |
|---|---|---|
| GET | `list` | `{"success": true, "count", "max_controllers", "controllers": [{"id", "name", "enabled", "kind", "mode", "sensor_id", "heat_relay_id", "cool_relay_id", "running"}, ...]}` |
| GET | `get?id=` | One thermostat, in the file format of [climate_hub](../climate_hub/README.md#a-thermostat) |
| POST | `save` | A thermostat as a JSON body. `id` absent or `""` creates one, an existing `id` replaces that one, renamed or not. Answers `{"success": true, "message", "id"}` |
| POST | `delete?id=` | Stops the thermostat and removes its file |
| POST | `enable?id=&value=true\|false[&take_over=true]` | Starts or stops it and stores the flag. `{"success": true, "message", "persisted"}`; `persisted` is `false` when it runs but the flag did not reach flash |
| POST | `setpoint?id=&value=` | Moves the target, clamped into the thermostat's range, whether it runs or not |
| GET | `status[?id=]` | `{"success": true, "controllers": [...]}`: per thermostat whether it runs, what it does (`off`, `idle`, `heating`, `cooling`), its fault, the room temperature and its age, the target and range, the bang-bang switching points, the heat and cool duty and relay state, and a running PID's terms. A stopped thermostat still reports its sensor's reading |
| GET | `entities` | `{"success": true, "sensors": [{"object_id", "name", "unit"}], "switches": [{"object_id", "name", "claimed_by"}]}`; `claimed_by` is the id of the running thermostat that holds the relay, or `""`. Internal entities are left out |
| GET | `schema` | The kinds, modes and faults, `max_controllers`, `name_max_length`, and every tunable number with its label, unit, default, range, step and hint, grouped as a form shows them: the table the device clamps against |
| GET | `ping` | `{"status": "ok"}` |

`save` reads a raw JSON body, so it needs a `Content-Type` that is not a form:
`application/json`. A form body (`application/x-www-form-urlencoded`, or no `Content-Type` at
all) is parsed into fields by the server instead: up to 1024 bytes it is answered
`Empty request body`, and a longer one gets the server's own bare `400` before it reaches this
API. A JSON body over 8 KiB is `413`. A missing number takes its default and every number is
clamped into its range, so a partial document is accepted. The refusals come in this order, and
the first one a document meets is the answer:

1. `400`: the body is empty or not JSON, or the document breaks a rule of the file format — the
   structure first, the name rules last (`Name is required`, `Name cannot contain '/'`, …);
2. `404` for an `id` no thermostat has, or `507` when a create would pass `max_controllers`;
3. `409` for a name another thermostat or a YAML climate answers to, compared without case and
   extra spaces or by the entity id both would get (`Room 1` and `Room_1`);
4. for an enabled thermostat, `400` when its sensor or a relay is not on the device, then `409`
   when a running thermostat holds its relay: `"Relay 1" is already driven by "Living Room"`.

`enable` refuses the same two ways when it starts a thermostat. With `take_over=true` the
thermostat holding the relay is stopped and stored as disabled first, in the same step, and the
answer names it: `Thermostat enabled; "Living Room" stopped`.

An `id` is the thermostat's slug (`a-z`, `0-9`, single dashes, at most 48): a missing one is
`Missing id parameter`, anything else `Invalid id parameter`. `value` and `take_over` of
`enable` are `true` or `false` exactly, and `value` of `setpoint` a plain decimal number; `nan`,
`inf`, hex and padding are refused. A `+` in it goes as `%2B`: the server decodes a bare `+`
to a space.

Every failure is `{"success": false, "error"}`, with the sentence an editor shows: `400` for a
bad request, `404` for an unknown `id` or path, `405` for the wrong method, `409` for a name or
a relay in use, `413` for an oversized body, `500` when nothing could be written, `503` when the
loop was busy and `507` at `max_controllers`. A `500` changed nothing: `The thermostat's file
could not be written`, or `Thermostat storage is not available` when the storage was not usable
at boot — then every write gets it, a save before its body is even read. The same contract,
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
refused, which collide, which id a create gets), and `client/mock/climateMock.ts` a
dependency-free in-memory implementation of the same routes, with a simulated room per sensor,
for a dev server or unit tests. They are the contract a browser client codes against and they
live here so they change with the C++ that they mirror. Nothing in this repository builds or
type-checks them yet.

## Testing

`python tests/run.py web_climate_editor` builds the handler for the ESPHome `host` platform
over the real hub, a directory standing in for the flash and the harness's `web_server_base`
stand-in, and drives every route through it; the cases are in
`tests/components/web_climate_editor/`. See [doc/TESTING.md](../../doc/TESTING.md). What the
host cannot reach is ESP-IDF's side of the answer: the status lines written by hand and the
hop from the server's task to the loop task.
