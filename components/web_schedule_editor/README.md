# web_schedule_editor

The JSON/REST API of the [`schedules`](../schedules/README.md) engine, served on the device's own
web server under `/schedule-editor/api/`. List, read, create, change, enable, disable and delete
weekly schedules — modes of relay and thermostat actions, picked by switch points — bring one
back from a backup under its own id, and list the relays and thermostats an action may drive —
everything a browser editor does, and everything `curl` can do without one. ESP-IDF only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, schedules, loop_job, switch_hold, web_origin_guard, web_schedule_editor]

web_server:
  port: 80

littlefs_storage:
  id: user_storage

time:
  - platform: sntp
    id: sntp_time

schedules:
  id: schedules_engine
  storage: user_storage
  time_id: sntp_time

web_schedule_editor:
```

## Options

| Option         | Default             | Meaning                                                                |
| -------------- | ------------------- | ---------------------------------------------------------------------- |
| `schedules_id` | the only `schedules` | The engine to edit                                                    |
| `url_prefix`   | `/schedule-editor`  | The routes live at `<url_prefix>/api/`. A path web_server answers itself (`/climate`, `/switch`, `/events`, …), anything overlapping `/api/device`, and a path on or below another component's `url_prefix` are refused |

The handler registers on the shared `web_server_base`, so it answers on the same port as
`web_server` and behind its `auth:` credentials. Those credentials come from the `web_server:`
block and from nowhere else: with no `web_server:`, or no `auth:` in it, anything that can reach
the port can rewrite every schedule and, through them, switch the relays and thermostats their
actions drive. The configs in this repository set them, and [`web_auth`](../web_auth/README.md) lets the
device replace the pair. Every route is also behind
[`web_origin_guard`](../web_origin_guard/README.md), which answers `403` to a request whose
`Origin` is not the `Host` it was sent to. Clients that send no `Origin`, `curl` among them, are
unaffected.

## REST

Routes match on the exact URL path under `<url_prefix>/api/`, and each answers the one method
below. A `GET` or `POST` the route does not take, and an `OPTIONS`, is `405` with an `Allow`
header naming the one it does. Any other path under the prefix is `404`.

Every route but `schema` and `ping` runs its whole read or write on the loop task and answers
`503 Service Unavailable` when the loop does not get to it within five seconds. Nothing was read
or written then, and the call can simply be made again. A read whose answer does not fit the
free memory is `503` too, `Device is low on memory; try again`.

| Method | Path | |
|---|---|---|
| GET | `list` | `{"success": true, "count", "max_schedules", "clock": {"day", "time"}, "schedules": [{"id", "revision", "name", "enabled", "targets": [{"type", "id", "name"}], "status": {"state", "reason", "note"?, "issues": [{"mode", "action", "kind", "text", "held_by"?}]}, "now", "next"}, ...]}`; `clock` is the device's local day and time and `now`/`next` are `{"day", "at", "mode"}`, all three `null` until the device has a valid time |
| GET | `get?id=` | One schedule, as its file holds it: `{"version", "revision", "id", "name", "enabled", "modes": [{"id", "name", "color"?, "actions": [...]}], "week": {"mon": [{"at", "mode"}], ...}}` |
| POST | `save` | A schedule as a JSON body. `id` absent or `""` creates one, an existing `id` replaces that one, renamed or not. `{"success": true, "message", "id"}`, a `warning` when it was saved enabled with an action the device skips, `started` naming the schedules that took over a target it left |
| POST | `import` | A schedule as `get` answers it, `id` included: the schedule with that `id` is replaced, otherwise one is created under it, enabled or not as the document says; its `revision` is not read. Answers as `save` does |
| POST | `delete?id=` | Removes the schedule and its file; its list stops and its targets stay as they are. `{"success": true, "message", "persisted"}`, and `started` |
| POST | `enable?id=&value=true\|false[&take_over=true]` | Enables or disables it and stores the flag. `{"success": true, "message", "persisted"}`, a `warning` as from `save`, `stopped` for what a take-over disabled, `started` |
| GET | `targets` | `{"success": true, "relays": [{"object_id", "name", "held_by", "scheduled_by"}], "thermostats": [{"id", "name", "enabled", "running", "mode", "min_temperature", "max_temperature", "step", "scheduled_by", "presets": [{"key", "name", "setpoint", "mode"}]}]}`; `thermostats` is `[]` without `climate_hub` |
| GET | `schema` | `{"version", "max_schedules", "max_points_per_day", "name_max_length", "max_modes", "max_actions", "mode_name_max_length", "mode_id_max_length", "min_wait_seconds", "max_wait_seconds", "days", "colors", "values": {"relay": ["on", "off"], "thermostat": ["on", "off", "preset", "target"]}}` |
| GET | `ping` | `{"status": "ok"}` |

A schedule's targets are every relay and thermostat any of its modes names. `status.state` is
the first that holds of `disabled`, `waiting_for_clock`, `conflict` (a schedule before it by id
drives one of its targets) and `running`; `reason` says why in a sentence, `""` when it runs or
is disabled. `issues` lists, whatever the state, each action the device would skip now: its
mode's id, its index in the mode's `actions`, the `kind` (`missing_relay`,
`missing_thermostat`, `missing_preset`, `no_thermostats`, or `held` for a relay a running
thermostat drives, which `held_by` names) and a sentence. `note` is there while it runs and names
a thermostat that is switched off: the thermostat takes the values and starts in them.

A schedule's `revision` is `0` when it is created and moves on by one at every change: a `save`,
an `enable` that changes the flag or stops it in a take-over, an `import` over it, a rename at
boot. A `save` of an existing schedule whose body has a `revision` other than the device's is
`409`, `The device changed this schedule since it was read; reload it`: another client changed it
since the form was read, and the Save would undo that. One without a `revision` is not checked,
and a create ignores it.

`save` and `import` read a raw JSON body, so they need `Content-Type: application/json`. A body
over 16 KiB is `413`. The refusals, in order: `400` for a body that is empty or not JSON or a
document that breaks a rule; `404` for an unknown `id` on `save`, `409` for a schedule whose file
a newer firmware wrote, `409` for a stale `revision`, `507` past `max_schedules`; `409` for a name
another schedule has; `409` for an enabled schedule that shares a target with another enabled
schedule (`"Winter" already drives Living Room; switch it off first or take over`, the first
shared target). `507`, `Not enough free memory on the device to save this schedule`, when the body
or the schedule would leave the device too little memory; the file and the schedule stay as they
were. A relay, a thermostat or a preset the device does not have is stored, with a
`warning` naming the first action it skips (`skips mode "Day", action 2: relay "relay_7" is not
on this device`).

The full contract, with every message, is [openapi.yaml](openapi.yaml); the TypeScript client
and mock are in [client/](client/).

## curl

```bash
DEV=http://device.local/schedule-editor/api
AUTH="--digest -u admin:password"

curl -s $AUTH "$DEV/list"
curl -s $AUTH "$DEV/targets"

# Porch light on at 19:00 and off at 23:30 on weekdays; the device makes the id from the name.
curl -s $AUTH -X POST -H 'Content-Type: application/json' "$DEV/save" -d '{
  "name": "Porch light",
  "modes": [
    {"id": "on", "name": "On", "color": "green",
     "actions": [{"type": "relay", "id": "relay_3", "value": {"type": "on"}}]},
    {"id": "off", "name": "Off", "color": "grey",
     "actions": [{"type": "relay", "id": "relay_3", "value": {"type": "off"}}]}
  ],
  "week": {
    "mon": [{"at": "19:00", "mode": "on"}, {"at": "23:30", "mode": "off"}],
    "tue": [{"at": "19:00", "mode": "on"}, {"at": "23:30", "mode": "off"}],
    "wed": [{"at": "19:00", "mode": "on"}, {"at": "23:30", "mode": "off"}],
    "thu": [{"at": "19:00", "mode": "on"}, {"at": "23:30", "mode": "off"}],
    "fri": [{"at": "19:00", "mode": "on"}, {"at": "23:30", "mode": "off"}]
  }
}'

# Comfort and the boiler pump in the morning, the valve two minutes later; eco and both off at night.
curl -s $AUTH -X POST -H 'Content-Type: application/json' "$DEV/save" -d '{
  "name": "Home weekdays",
  "modes": [
    {"id": "day", "name": "Day", "color": "amber", "actions": [
      {"type": "thermostat", "id": "living-room", "value": {"type": "preset", "preset": "comfort"}},
      {"type": "relay", "id": "relay_1", "value": {"type": "on"}},
      {"type": "wait", "seconds": 120},
      {"type": "relay", "id": "relay_5", "value": {"type": "on"}}]},
    {"id": "night", "name": "Night", "color": "indigo", "actions": [
      {"type": "thermostat", "id": "living-room", "value": {"type": "preset", "preset": "eco"}},
      {"type": "relay", "id": "relay_5", "value": {"type": "off"}},
      {"type": "relay", "id": "relay_1", "value": {"type": "off"}}]}
  ],
  "week": {"mon": [{"at": "06:30", "mode": "day"}, {"at": "23:00", "mode": "night"}]}
}'

curl -s $AUTH "$DEV/get?id=porch-light" > porch-light.json
curl -s $AUTH -X POST -H 'Content-Type: application/json' "$DEV/import" -d @porch-light.json
curl -s $AUTH -X POST "$DEV/enable?id=porch-light&value=false"
curl -s $AUTH -X POST "$DEV/enable?id=winter&value=true&take_over=true"
curl -s $AUTH -X POST "$DEV/delete?id=porch-light"
```
