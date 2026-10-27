# schedules

Weekly time programs the device runs without a recompile. Each schedule names modes, lists of
actions on relays and [climate_hub](../climate_hub/README.md) thermostats with waits between
them, and a week of switch points that pick a mode: at each point the mode's list runs, and the
mode stays in force until the next one. Schedules are JSON files on a filesystem storage, and
can be added, changed, switched on and off and removed while the device runs.

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

The `id:` cannot be `schedules`, the component's C++ namespace. `climate_hub` is optional;
without it an action on a thermostat is kept but skipped.

## A schedule

```json
{"version": 1, "revision": 4, "id": "home-weekdays", "name": "Home weekdays", "enabled": true,
 "modes": [
  {"id": "day", "name": "Day", "color": "amber", "actions": [
    {"type": "thermostat", "id": "living-room", "value": {"type": "preset", "preset": "comfort"}},
    {"type": "relay", "id": "boiler_pump", "value": {"type": "on"}},
    {"type": "wait", "seconds": 120},
    {"type": "relay", "id": "relay_5", "value": {"type": "on"}}]},
  {"id": "night", "name": "Night", "color": "indigo", "actions": [
    {"type": "thermostat", "id": "living-room", "value": {"type": "preset", "preset": "eco"}},
    {"type": "relay", "id": "relay_5", "value": {"type": "off"}},
    {"type": "relay", "id": "boiler_pump", "value": {"type": "off"}}]}],
 "week": {
  "mon": [{"at": "06:30", "mode": "day"}, {"at": "23:00", "mode": "night"}],
  "tue": [], "wed": [], "thu": [], "fri": [], "sat": [], "sun": []}}
```

- `modes`: 1 to 8. A mode's `id` is 1 to 24 of `a-z`, `0-9` and `-`, unique in the schedule;
  points name the mode by it, so a rename keeps it. Its `name` is 1 to 24 characters, any
  language, no control characters, unique in the schedule whatever the case and spacing.
  `color` is optional, one of `red`, `orange`, `amber`, `yellow`, `lime`, `green`, `teal`,
  `cyan`, `blue`, `indigo`, `violet`, `pink`, `grey`. `actions`: 0 to 12, in order; left out
  reads as none.
- An action: `{"type": "relay", "id": "<object id>", "value": {"type": "on" | "off"}}`;
  `{"type": "thermostat", "id": "<thermostat id>", "value": V}`, V one of `{"type": "on"}`,
  `{"type": "off"}`, `{"type": "preset", "preset": "<preset key>"}`,
  `{"type": "target", "target": <°C>}`; `{"type": "wait", "seconds": 1..86400}`.
- `week`: the days `mon` to `sun`, each a list of up to 12 points, `at` as `HH:MM` from `00:00`
  to `23:59` and `mode` a mode's id. A day left out has no points; the week needs at least one.
  Before a week's first point, the week's last one holds: Sunday evening carries into Monday
  morning.
- `name`: 1 to 48 characters, any language, no control characters, neither `/` nor `\`; unique
  on the device whatever the case and spacing.
- `id`: made from the name at creation (`schedule`, `schedule-2` for a name with no Latin letters
  or digits), never `new`, kept through a rename.
- `revision`: `0` when created, and one more at every change of the document: an `update()`, a
  `set_enabled()` that changes the flag, a take-over that disables it, a `restore()` over it, a
  rename at boot. A file without one reads as `0`.

A document is refused with the first rule it breaks, in the editor's order — the name, the
modes one at a time and each one's actions, then the week a day at a time — in its words:
`Name is required`, `A schedule has at most 8 modes`, `Mode 2: name is required`,
`Two modes are called "Day"`, `Mode "Day": at most 12 actions`, `Mode "Day": color must be one
of red, orange, …`, `Mode "Day", action 2: a relay takes on or off`, `Mode "Day", action 3: a
wait is 1 s to 24 h`, `Unknown day "mo": expected mon, tue, wed, thu, fri, sat or sun`,
`Mon: 06:30 is there twice`, `Mon 06:30: there is no mode "dusk"`, `A schedule needs at least
one switch point` and the like.

## When a list runs

- At every switch point, from its first action, even when the targets already are that way; a
  hand change in between holds until the next point.
- The list of the mode in force runs again, waits included: once the clock is first valid after
  a boot, when the schedule is saved enabled (a rename too), when it is switched on, and when
  the schedule that drove one of its targets is switched off, deleted or moved elsewhere.
- When the clock moves by more than a minute against the time that passed: a clock set by hand
  or by SNTP, a new time zone, a DST switch. The list of the mode in force runs; points skipped
  are not replayed. When clocks go forward, the mode in force after the gap runs; when they go
  back, the points in the repeated hour run a second time. A smaller step never runs a point
  twice, and a step forward across a point runs it.
- After the loop was held up, only the latest point it missed runs.
- A new point stops a list in a wait. Switching a schedule off, deleting it or saving it onto
  other targets stops its list and leaves the targets as they are.

## Targets

- A schedule's targets are every relay and thermostat any of its modes names.
- An action on a relay a running thermostat drives is skipped (`held` among the issues, the log
  names the thermostat). Once the thermostat lets go, the relay takes the last action on it the
  running list has passed, or the whole list once it ended; none yet, and the list gets to it.
- A thermostat action works as a rule's: a preset that keeps the mode, or a target, turns a
  thermostat that is off on first; a preset with a mode of its own sets that mode; `on` goes
  back to the mode it had before off. A stopped thermostat takes the values into its document
  and stays stopped. A scheduled change ends a calibration.
- A relay, a thermostat or a preset not on the device is no error: the schedule is saved, with a
  `warning`, the action is skipped and the rest of the list runs. When a thermostat or a preset
  comes back, the actions on it the list has passed apply again, in order, without the waits.
- One enabled schedule per target. Saving another one enabled that shares a target is refused
  with 409 naming the first shared target, and so is switching one on, unless it takes over:
  every other one that shares a target is then stored disabled. Two enabled ones that share a
  target come only from files written by hand; then the first by id drives it, the other waits
  whole (`conflict`) and starts once the first is switched off, deleted or saved onto other
  targets.

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
- `status(id)`: `state`, the first that holds of `disabled`, `waiting_for_clock`, `conflict` and
  `running` (`state_to_string()`); the `reason` it does not run (`Waiting for the clock`,
  `Schedule "Winter" drives Living Room`); a `note` for a thermostat that is switched off; and
  `issues`, each action of each mode the device would skip now — its mode and action index,
  `kind` (`missing_relay`, `missing_thermostat`, `missing_preset`, `no_thermostats`, `held`;
  `issue_kind_to_string()`), `text` (`Relay "relay_7" is not on this device`,
  `Thermostat "Floor" drives Relay 3`) and `held_by`
- `target_name(target)`: the relay's or the thermostat's name, its id when it is not there
- `now_and_next(config, &now, &next)`: the point in force and the next one, as a day (0 is
  Monday), minutes since midnight and the `Point`, whose `mode` indexes `config.modes`; false
  until the clock has been valid
- `clock_text(&day, &time)`: the device's local day (`wed`) and time (`10:15:42`)
- `relay_targets()`: every visible switch, with the thermostat that holds it (`held_by`) and the
  schedule that drives it (`scheduled_by`); `thermostat_targets()`: every thermostat with its
  name, enabled and running, mode, range, step, presets (key, name, setpoint, mode or `keep`) and
  `scheduled_by`, none without `climate_hub`; `scheduled_by(type, id)`
- `create(draft)`, `update(id, doc, revision)`, `restore(doc)`, `remove(id)`,
  `set_enabled(id, enabled, take_over)`: each returns a `Result` — `ok`, the HTTP `code` that
  fits (400, 404, 409, 413, 500, 507), an `error` sentence, the new `id`, the `holder` of a 409
  over a target, a `warning` when it was saved enabled with an action the device skips
  (`skips mode "Day", action 2: relay "relay_7" is not on this device`), `persisted`, the ids a
  take-over `stopped` and the ids that `started` because the change let them drive their
  targets. `restore()` brings a schedule back under the id its document names, as a backup holds
  it, replacing or creating it, and refuses an id that is no slug or is `new` (400) or that a file
  the boot refused holds (409). `update()` refuses a `revision`, when one is given, that is not
  the stored one, with 409 `The device changed this schedule since it was read; reload it`; the
  `revision` a document brings is ignored by all three
- `ScheduleConfig::deserialize(json, require_id, &error)`, `validate(&error)`, `serialize(json)`;
  `add_mode()`, `add_relay()`, `add_thermostat()`, `add_wait()` and `add_point()` build one in C++

## Testing

`python tests/run.py schedules schedules_no_climate` builds the component for the ESPHome `host`
platform into two Google Test binaries, with and without `climate_hub`, with a directory standing
in for the flash and clocks the tests move. See [doc/TESTING.md](../../doc/TESTING.md).
