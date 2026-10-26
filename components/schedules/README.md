# schedules

Weekly time programs the device runs without a recompile. Each schedule drives one target, a
relay or a [climate_hub](../climate_hub/README.md) thermostat, through a week of switch points:
from each point on, until the next one, the target takes the point's value. Schedules are JSON
files on a filesystem storage, and can be added, changed, switched on and off and removed while
the device runs.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, schedules, loop_job, switch_hold]

littlefs_storage:
  id: user_storage

time:
  - platform: pcf8563
    id: pcf8563_time

schedules:
  id: schedules_engine
  storage: user_storage
  time_id: pcf8563_time
```

## Options

| Option          | Default     | Meaning                                                                    |
| --------------- | ----------- | -------------------------------------------------------------------------- |
| `storage`       |             | A `filesystem_storage_abstract` backend, `littlefs_storage` on a device; required |
| `time_id`       |             | The clock the week is read from, in local time; required                   |
| `folder_path`   | `schedules` | The folder below the backend's base path that holds the schedule files; not `climate_hub`'s or `automations`' folder on the same storage |
| `max_schedules` | `16`        | 1 to 32: how many schedules there can be                                   |

The `id:` cannot be `schedules`, the component's C++ namespace. `climate_hub` is optional; without it a schedule for a thermostat is kept but does
not run.

## A schedule

```json
{"version": 1, "id": "living-room-week", "name": "Living room week", "enabled": true,
 "target": {"type": "thermostat", "id": "living-room"},
 "week": {
  "mon": [{"at": "06:30", "value": {"type": "preset", "preset": "comfort"}},
          {"at": "08:30", "value": {"type": "preset", "preset": "eco"}},
          {"at": "17:00", "value": {"type": "target", "target": 21.5}},
          {"at": "23:00", "value": {"type": "off"}}],
  "tue": [], "wed": [], "thu": [], "fri": [], "sat": [], "sun": []}}
```

- `target`: `{"type": "relay", "id": "<object id>"}` or `{"type": "thermostat", "id": "<thermostat id>"}`.
- `week`: the days `mon` to `sun`, each a list of up to 12 points, `at` as `HH:MM` from `00:00`
  to `23:59`. A day left out has no points; the week needs at least one. Before a week's first
  point, the week's last one holds: Sunday evening carries into Monday morning.
- A relay takes `{"type": "on"}` or `{"type": "off"}`. A thermostat takes
  `{"type": "preset", "preset": "<preset key>"}`, `{"type": "off"}` or
  `{"type": "target", "target": <°C>}`; the thermostat clamps a target into its range.
- `name`: 1 to 48 characters, any language, no control characters, neither `/` nor `\`; unique
  on the device whatever the case and spacing.
- `id`: made from the name at creation (`schedule`, `schedule-2` for a name with no Latin letters
  or digits), never `new`, kept through a rename.

A document is refused with the first rule it breaks, in the editor's words: `Name is required`,
`target.type must be relay or thermostat`, `Unknown day "mo": expected mon, tue, wed, thu, fri,
sat or sun`, `Mon: at most 12 switch points a day`, `Mon: at must be HH:MM from 00:00 to 23:59`,
`Mon: 06:30 is there twice`, `A schedule needs at least one switch point`, `Mon 06:30: a relay
takes on or off`, `Mon 06:30: unknown value type "heat"`, `Mon 06:30: preset must be a preset's
key`, `Mon 06:30: target must be a number` and the like.

## When a value applies

- At every switch point, once, even when the target already has that value; a hand change in
  between holds until the next point.
- When the schedule starts: once the clock is first valid after a boot, when it is saved enabled
  (a rename too), when it is switched on, when its target or preset comes back, and when the
  schedule that drove its target is switched off, deleted or moved elsewhere. Then the value in
  force applies at once.
- When the clock moves by more than a minute against the time that passed: a clock set by hand
  or by SNTP, a new time zone, a DST switch. The value in force applies; points skipped are not
  replayed. When clocks go forward, the value in force after the gap applies; when they go back,
  the points in the repeated hour apply a second time. A smaller step never applies a point
  twice, and a step forward across a point applies it.
- After the loop was held up, only the latest point it missed applies.
- Switching a schedule off or deleting it leaves its target as it is; so does saving it onto
  another target.

## Targets

- A relay a running thermostat drives is left alone (`held`), and takes the value in force once
  the thermostat lets go of it.
- A thermostat takes a preset, a target or off as from an automation rule: a preset that keeps
  the mode, or a target, turns a thermostat that is off on first; a preset with a mode of its own
  sets that mode. A stopped thermostat takes the values into its document and stays stopped.
  A scheduled change ends a calibration.
- A target or a preset not on the device is no error: the schedule is saved, with a `warning`,
  and waits until it is there.
- One enabled schedule per target. Saving another one enabled on it is refused with 409, and so
  is switching one on, unless it takes over: the others are then stored disabled. Two enabled
  ones on a target come only from files written by hand or a restore; then the first by id
  drives it and the other waits (`conflict`), and starts once the first is switched off, deleted
  or saved onto another target.

## Storage

One file per schedule, `<base_path>/<folder_path>/<id>.json`, written beside the old one and
renamed over it, so a failed write leaves the previous file whole. Nothing is written at a
switch point.

The folder is writable by hand, so what it holds is checked at boot:

- at most `max_schedules` files are loaded, in file name order; the rest are left alone;
- a file whose `id` is not its file name or is `new`, that is not valid JSON, over 16 KiB, or
  that breaks a rule above is refused and left exactly as it is, and its id is not given to a
  new schedule nor taken by a restore;
- a name another schedule already has becomes `<name> 2` and is written back;
- a file whose `version` is higher than this firmware's came from a newer one: it loads and runs,
  but is never written. `update()` and `restore()` refuse it with 409, `A newer firmware wrote
  this schedule; update the firmware to change it`; switching it on or off lasts until the next
  boot (`persisted` false), and a take-over by it leaves the others' files enabled. `remove()`
  still deletes it.

A removal the partition refuses empties the file instead, so the next boot refuses it; when
that fails too, `remove()` returns `persisted` false.

## From C++

Everything runs on the loop task: a caller on another task (an ESP-IDF HTTP handler) wraps its
whole read, decide and write in one `run_on_loop(job)`, which blocks and returns `false` when the
loop never got to it. `is_failed()` is true when the storage was not mounted or the folder could
not be read; every change is then refused with 500.

- `schedules()`, `get(id)`: the documents, sorted by id; `document(id)`: one as its file holds
  it, which `deserialize()` and `restore()` take back; `max_schedules()`
- `status(id)`: `state`, the first that holds of `disabled`, `waiting_for_clock`,
  `missing_target`, `missing_preset`, `conflict`, `held` and `running` (`state_to_string()`),
  the `reason` it does not drive its target (`Waiting for the clock`, `Relay "relay_7" is not on this device`, `Thermostat "attic" is not
  on this device`, `This device has no thermostats`, `Thermostat "Living Room" has no preset
  "eco"`, `Schedule "Winter" drives this target`, `Thermostat "Floor" drives Relay 3`), `held_by`,
  and a `note` for a thermostat that is switched off
- `target_name(config)`: the relay's or the thermostat's name, its id when it is not there
- `now_and_next(config, &now, &next)`: the point in force and the next one, as a day (0 is
  Monday), minutes since midnight and the `Point`, whose value `config.write_value(obj, point)`
  writes as the file does; false until the clock has been valid
- `clock_text(&day, &time)`: the device's local day (`wed`) and time (`10:15:42`)
- `relay_targets()`: every visible switch, with the thermostat that holds it (`held_by`) and the
  schedule that drives it (`scheduled_by`); `thermostat_targets()`: every thermostat with its
  name, enabled and running, mode, range, step, presets (key, name, setpoint, mode or `keep`) and
  `scheduled_by`, none without `climate_hub`
- `create(draft)`, `update(id, doc)`, `restore(doc)`, `remove(id)`,
  `set_enabled(id, enabled, take_over)`: each returns a `Result` — `ok`, the HTTP `code` that
  fits (400, 404, 409, 413, 500, 507), an `error` sentence, the new `id`, the `holder` of a 409
  over a target, a `warning` when it was saved enabled but does not run, `persisted`, the ids a
  take-over `stopped` and the ids that `started` because the change let them drive their target.
  `restore()` brings a schedule back under the id its document names, as a backup holds it,
  replacing or creating it, and refuses an id that is no slug or is `new` (400) or that a file the
  boot refused holds (409)
- `ScheduleConfig::deserialize(json, require_id, &error)`, `validate(&error)`, `serialize(json)`

## Testing

`python tests/run.py schedules schedules_no_climate` builds the component for the ESPHome `host`
platform into two Google Test binaries, with and without `climate_hub`, with a directory standing
in for the flash and clocks the tests move. See [doc/TESTING.md](../../doc/TESTING.md).
