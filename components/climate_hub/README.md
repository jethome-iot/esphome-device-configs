# climate_hub

Thermostats the device runs without a recompile. Each one is a JSON file on a filesystem
storage that names a temperature sensor and one or two relays, and runs a hysteresis
(bang-bang) or a PID control law on them. Every running thermostat is a climate entity that
Home Assistant and the web server show and control like any other. Thermostats can be added,
changed, started, stopped and removed while the device runs.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, climate_hub, loop_job, switch_hold]

littlefs_storage:
  id: user_storage

one_wire:
  - platform: gpio
    pin: GPIO4

sensor:
  - platform: dallas_temp
    name: "Room"

switch:
  - platform: gpio
    pin: GPIO16
    name: "Boiler"

web_server:
  version: 3
  sorting_groups:
    - id: group_climate
      name: "Thermostats"

climate_hub:
  storage: user_storage
  max_controllers: 8
  web_server:
    sorting_group_id: group_climate
```

## Options

| Option            | Default    | Meaning                                                                  |
| ----------------- | ---------- | ------------------------------------------------------------------------ |
| `storage`         |            | A `filesystem_storage_abstract` backend, `littlefs_storage` on a device; required |
| `folder_path`     | `climates` | The folder below the backend's base path that holds the thermostat files |
| `max_controllers` | `8`        | 1 to 16: how many thermostats there can be, and how many climate entities are set aside for them |
| `web_server`      |            | `sorting_group_id` and `sorting_weight` for the web server's entity list, as on any entity |

ESP32 and the host platform only. `web_server: include_internal: true` is refused next to the
component: it would list the entities no thermostat is using.

## A thermostat

```json
{
  "version": 3,
  "revision": 0,
  "id": "living-room",
  "name": "Living room",
  "enabled": true,
  "kind": "bang_bang",
  "sensor_id": "temp_1",
  "update_interval_s": 30,
  "heat": {"relay_id": "relay_1", "period_s": 300, "min_on_s": 10, "min_off_s": 10},
  "cool": {"relay_id": "", "period_s": 300, "min_on_s": 10, "min_off_s": 10},
  "visual": {"min_temperature": 5, "max_temperature": 45, "step": 0.5},
  "safety": {"sensor_timeout_s": 300, "max_temperature": 60},
  "pid": {"kp": 0.6, "ki": 0.0025, "kd": 0, "min_integral": -1, "max_integral": 1,
          "starting_integral_term": 0, "output_samples": 1, "derivative_samples": 8,
          "deadband_threshold_low": 0, "deadband_threshold_high": 0,
          "deadband_kp_multiplier": 0, "deadband_ki_multiplier": 0, "deadband_kd_multiplier": 0,
          "deadband_output_samples": 1},
  "bang_bang": {"below": 0.5, "above": 0.5},
  "mode": "heat",
  "setpoint": 21,
  "presets": [
    {"key": "eco", "name": "Eco", "setpoint": 18, "mode": "keep"},
    {"key": "night", "name": "Night", "setpoint": 19.5, "mode": "heat"}
  ],
  "active_preset": "eco"
}
```

| Key                  | Values                                                                     |
| -------------------- | -------------------------------------------------------------------------- |
| `version`            | `3`, the format this firmware writes; a file without one, or with `1` or `2`, is read as `3` and written back as `3`. A higher one is a file from a newer firmware, see [Storage](#storage) |
| `revision`           | `0` when created; moves on only when a [calibration](#calibration) writes new gains, never on an `update()` or any other write. A file without one reads as `0` |
| `id`                 | Made from the name when the thermostat is created (`a-z`, `0-9`, single dashes, at most 48; `New` gets `new-2`, since the dashboard opens a blank editor at `new`), then never changes; the file is `<id>.json` |
| `name`               | 1 to 48 printable ASCII characters, neither `/` nor `\`, trimmed; also the climate entity's name |
| `kind`               | `bang_bang` (the default) or `pid`                                         |
| `sensor_id`          | The object id of a temperature sensor that reports °C, `temp_1` for `Temp 1`; at most 120 characters, the longest an object id gets |
| `heat`, `cool`       | `relay_id`: the object id of a switch, `""` for a direction not used; at least one, not the same one twice, at most 120 characters |
| `mode`               | `off`, `heat`, `cool` or `heat_cool`; a mode needs the relays it drives    |
| `setpoint`           | The one target, held inside `visual.min_temperature` … `visual.max_temperature` |
| `bang_bang`          | The switching points sit `below` and `above` the target                   |
| `presets`            | Up to 8, in the order Home Assistant lists the custom ones; see [Presets](#presets) |
| `active_preset`      | The `key` of the preset picked last, `""` for none; one no preset has is read as `""` |

Every number is clamped into its range, and a missing one takes its default: the ranges and the
defaults are the table in `param_table.cpp`. A document built in C++ and handed to `create()` or
`update()` is clamped the same way. A document that breaks a rule above is refused
whole with a sentence that says which. A thermostat that is to run is created, saved or enabled
only when its sensor, if it is on the device, reports °C, and no other enabled thermostat names
its relays: neither a running one, which holds them, nor one that waits, which reserves them. A
relay it holds already is never refused. One whose sensor or a relay is not on the device is
saved or enabled all the same, and waits as it would at boot, with a `warning` that names what
is missing. A disabled one may name what is not there yet.

## Presets

| Key        | Values                                                                         |
| ---------- | ------------------------------------------------------------------------------ |
| `key`      | The slug of the name the preset was created with (`a-z`, `0-9`, single dashes, at most 48), `-2` and on when another preset has it; a rename keeps it. A file may leave it out: it is then made from the name |
| `name`     | The thermostat name rules; `none`, in any case, is reserved. Unique within the thermostat, compared without case and extra spaces |
| `setpoint` | Required; held inside the visual range like the thermostat's own                |
| `mode`     | `keep` (the default) leaves the thermostat's mode, or `off`, `heat`, `cool`, `heat_cool`, which need the relays they drive |

A name equal to one of Home Assistant's standard presets in any case (`eco`, `away`, `boost`,
`comfort`, `home`, `sleep`, `activity`) is that built-in preset there; any other name is a custom
preset, listed by its name. Two names Home Assistant would read as one (`Eco` and `ECO`) clash.
A preset that breaks a rule refuses the whole document, with a sentence that names its place in
the list: `Preset 2: "none" is reserved`. The presets' rules come after the thermostat's own and
before its name rules.

- **A pick** from Home Assistant, the web server or `apply_preset()` sets the preset's target and,
  unless it says `keep`, its mode, and makes it the active preset. A call that carries a target
  or a mode besides takes the preset first and those over it.
- **A target or a mode set by hand** keeps the label, as ESPHome's own thermostat does.
- **The active preset** is written to the file with the target and the mode, and shown again
  after a reboot; its values are not applied again, so a target set by hand since stays.
- **A create** makes every key from its preset's name and starts with no preset active.
- **A Save** keeps the key of every preset it brings back, makes one from the name for a new
  preset, and ignores `active_preset`: the active preset stays while its key is in the list,
  and goes when its preset is removed. New values for the active preset apply at once.

## Control

- **Hysteresis.** Heating starts below `setpoint - below` and stops above `setpoint + above`;
  cooling the other way round. Between the two points the relay keeps what it was doing.
- **PID.** ESPHome's `pid` law, run every `update_interval_s`, drives each relay as a slow PWM
  over `period_s`: positive output heats, negative cools.
- A change of mode or target takes effect at the next loop pass, not at the next interval.
- **Safety.** A thermostat keeps its relays open until its first reading, idle and without a
  fault, and opens one closed from elsewhere meanwhile again on every pass. With no reading
  within `sensor_timeout_s` of its start it reports `sensor_stale`. It opens them as well when
  its sensor has been silent for `sensor_timeout_s`, while the reading is above
  `safety.max_temperature`, and in mode `off`. A thermostat that starts shows its sensor's last
  value at once, but acts on it only if it arrived within `sensor_timeout_s` while a thermostat
  was running on that sensor, and the timeout runs from that reading; otherwise it waits for the
  next one. A reading that is not a finite number, `NaN` or an infinity, is no reading: it is
  neither shown nor acted on, so a thermostat whose sensor sends nothing else reports
  `sensor_stale` once `sensor_timeout_s` has passed since its last good one.
- `min_on_s` and `min_off_s` hold a relay closed or open that long after it moved, whichever
  thermostat moved it or put it back; a safety cut-out does not wait for them. A relay no
  thermostat has held since boot counts as opened at boot, so `min_off_s` holds across a
  reboot. Keeping an open relay open, as while a thermostat waits for its first reading, is no
  move.
- A Save keeps what the thermostat is doing: inside the band a hysteresis thermostat goes on
  heating, cooling or idling as it was in the modes it still has, and the PWM keeps its rhythm
  unless `period_s` changes. Unless it changes `kind` or `sensor_id`, it also keeps what a PID
  has learnt: new gains apply from the next pass, and the integral is clamped into new limits.
  `starting_integral_term` applies when a thermostat starts and after a Save that changes
  either. A Save that keeps `sensor_id` does not restart the wait for a first reading.
- `kd` reaches 10000 and `ki` steps by a millionth, so the gains a slow floor calibrates to fit.
- The entity reports the room temperature to a tenth of a degree, the target in steps of
  `visual.step`, the mode and what it is doing: heating, cooling, idle, or off, which only
  mode `off`, a fault other than `relay_contested` or a stopped thermostat shows. Home
  Assistant and the web server can set the mode and the target; a target outside the range is
  clamped to it.

## Calibration

`start_autotune()` calibrates a running PID thermostat with ESPHome's relay-oscillation autotune:
the relay in the direction asked for closes fully `0.25` °C under the target and opens `0.25` °C
over it (a cooling relay the other way round), the other relay held open, `min_on_s`, `min_off_s`
and `safety.max_temperature` kept. Every reading feeds it, not every `update_interval_s`. At its
sixth relay switch it has the room's ultimate gain Ku and period Pu, the numbers ESPHome's
`climate.pid.autotune` gives a `pid` climate with a heat or a cool output alone (with both, it
swings the room between full heating and full cooling, and measures another swing), and the rule
asked for turns them into gains:

| Rule             | kp         | ki            | kd               |
| ---------------- | ---------- | ------------- | ---------------- |
| `zn_pi`          | 0.45 Ku    | 0.54 Ku / Pu  | 0                |
| `zn_pid`         | 0.6 Ku     | 1.2 Ku / Pu   | 0.075 Ku · Pu    |
| `pessen`         | 0.7 Ku     | 1.75 Ku / Pu  | 0.105 Ku · Pu    |
| `some_overshoot` | 0.333 Ku   | 0.667 Ku / Pu | 0.111 Ku · Pu    |
| `no_overshoot`   | 0.2 Ku     | 0.4 Ku / Pu   | 0.0625 Ku · Pu   |

Each gain is held in its range and written to the file as the file keeps it, and the `revision`
moves on; the thermostat goes on with them from a clean PID. Gains the file did not take run all
the same and are written again with the next flush, three seconds later, or at shutdown; the run
says `persisted()` false until a write of the thermostat succeeds. A room with radiators takes
about an hour and a half, a floor heating eight to ten hours. Start one with the room near its
target: until the first switch the relay stays full on (or off), and a floor that needs more than
6 hours to reach the band ends the run as `no_switch` before it has measured anything.

A run ends without gains on `cancel_autotune()`, a target or a mode changed from anywhere, an
`update()`, a stop or a take-over, any fault, `relay_contested` too, 24 hours in all, 6 hours
without a relay switch, or readings that cross the target more than 64 times (`noisy`, a probe
that hovers at the target); the thermostat goes back to its PID with the gains it had, from a
clean start. `autotune(id)` keeps the last run, running or ended, with the target it swung
around, the reason it ended, the extremes of its swings, Ku, Pu, the gains it replaced and wrote,
and its flags, until the next start, a `remove()` or a reboot. The flags warn and never extend a run: `asymmetric` (the shortest
half-period under 0.66 of the longest), `uneven` (the smallest swing under 0.66 of the largest)
and `clamped` (a gain held in its range).

## Relays

A running thermostat holds its relays, in mode `off` too, and only stopping it frees them. The
hub is the firmware's [`switch_hold`](../switch_hold/switch_hold.h) holder: whatever asks it
before moving a relay — the panel, the Modbus coils, automation rules, input bindings, the
relay's Inverted setting — leaves a held one alone, and `bindings` hears when a stop, a
removal, a take-over or a Save that drops a relay leaves it free.

What moves a held relay all the same, Home Assistant or the web server's REST API say, the
thermostat puts back. The first move goes back within a loop pass. From the second on, the
relay stays where it was moved until its `min_on_s` or `min_off_s` there is over, 10 s at
least, so a writer that keeps at it gets one switch per dwell. A relay moved where the
thermostat would switch it now stays and is not counted; moved there before the thermostat's
own `min_on_s` or `min_off_s` is over, it is not counted either: as the first move it goes
back, from the second on it stays. Once 5 moves come without 10 quiet minutes after a put-back
between them, the thermostat reports `relay_contested`: it goes on controlling and putting the
relay back, and the fault clears by itself 10 minutes after the last put-back. The count
starts over when a thermostat starts or takes the relay over, not at a Save, and a relay the
thermostat finds moved when it claims it, closed by Start mode On at boot say, counts as no
move. Until the first reading and during `sensor_stale` and `overtemp`, a relay closed from
elsewhere is opened again on every pass, without waiting. Mode `off` keeps holding the relays
open and puts them back the same way.

Two thermostats may name the same relay and take turns: only one of them is enabled at a time.
Enabling the second, or saving it enabled, while the first is enabled is refused, naming the
first, whether it runs or waits; unless it takes the relay over, which stores the first disabled
and stops it if it runs. A take-over by one whose sensor or a relay is not on the device is
refused, and nothing changes. Stopping a thermostat opens its relays; one that waits holds none
and moves none. A Save that keeps a relay leaves it where it is, and so does a take-over: a
relay both thermostats drive changes hands as it is, and the holder's other relays open.

Two enabled thermostats on one relay come only from files written by hand or a restore. The
boot runs the first by id and the other waits for the relay. It starts as soon as the relay is
free: when the holder is disabled or removed, saved onto other relays or onto a sensor that is
not there, or stopped by a take-over that does not want this relay. Of several waiting for it,
the first by id starts, and the next one's reason names that one. One that starts so has the
relay before `bindings` hears of it, which then never does.

## Names and Home Assistant

A thermostat's name is its entity's name. No two thermostats may share a name, compared without
case and extra spaces, or two names that give the same entity id (`Room 1` and `Room_1`), and a
thermostat may not take the name of a climate declared in YAML, `internal: true` or not.

Home Assistant knows an entity by its name. Renaming a thermostat therefore shows up there as a
new entity, and the old one becomes unavailable. A thermostat that stops or is removed drops out
of the entity lists; its entity keeps its name, and the web server still answers that name with
a stopped state, as it does for any `internal: true` entity, until another thermostat takes it.
A command sent to that name is accepted and does nothing. One that starts again under that name
gets its entity back.

The device asks Home Assistant to reconnect, so that it lists the entities again, when a
thermostat starts or stops (removing a running one stops it), and when a running one is
renamed, gains or loses its heating or cooling relay, gets a new temperature range or step,
gains or loses a built-in preset, or has a custom one added, removed, renamed or moved: the
whole device is unavailable there for about five seconds. A burst of such edits costs one
reconnect. Swapping one relay for another, a new target, mode, band or gains, new values for a
preset, a built-in preset's name in another case (`Eco` to `ECO`) or another place in the list,
and any change to a thermostat that is not running cost none.

## Storage

One file per thermostat, `<base_path>/<folder_path>/<id>.json`, written beside the old one and
renamed over it, so a failed write or a power cut leaves the previous file whole. A target or a
mode set from Home Assistant is written at most three seconds later, so a dragged slider costs
one write, and at shutdown.

The folder is writable by hand, so what it holds is checked at boot:

- at most `max_controllers` files are loaded, in file name order; the rest are left alone;
- a file whose `id` is not its file name or is `new`, that is not valid JSON, or that breaks a
  rule above is refused and left exactly as it is, and its id is not given to a new thermostat
  (a name whose every id is taken that way is refused);
- a name another thermostat or a YAML climate already has becomes `<name> 2` and is written back;
- an enabled thermostat whose sensor or relay is missing, whose sensor does not report °C, or
  whose relay another one holds, stays enabled and does not run; `waiting_reason()` says which;
- a file whose `version` is higher than this firmware's came from a newer one. It loads and
  runs as far as this firmware understands it, but is never written: a target, a mode, a preset
  or a stop from Home Assistant, the editor or a rule applies until the next reboot, a rename at
  boot too, and `update()` refuses it with 409, `A newer firmware wrote this thermostat; update
  the firmware to change it`. A take-over by it disables the others in memory only, the holder
  and the ones that wait alike: `set_enabled()` returns `persisted` false and their files stay
  enabled. `apply_preset()` returns `persisted` false when the pick changed something.
  `remove()` still deletes it. A newer file that breaks one of this firmware's rules (a preset
  `mode` it does not know, nine presets, a `kind` it does not have) is refused and left as it
  is, as any other file that does.

One that waits for a relay another thermostat holds starts when the relay is free (see
[Relays](#relays)). A sensor or a relay never turns up while the device runs, so one that waits
for it starts at the next boot that finds it, or at a Save or an enable once it is there.

A removal the partition refuses empties the file instead: the next boot refuses an empty file,
so the thermostat does not come back. When the file cannot be emptied either, the thermostat is
gone only until the next boot, which loads it from that file again; `remove()` then returns
`persisted` false.

## From C++

`esphome::global_climate_hub` is the component, and the `id:` names it in lambdas. Everything
runs on the loop task: a caller on another task (an ESP-IDF HTTP handler) wraps its whole read,
decide and write in one `run_on_loop(job)`, which blocks and returns `false` when the loop never
got to it.

- `store()`: the documents, sorted by id; `max_controllers()`
- `is_running(id)`, `runtime(id)`: the running thermostat's action, fault, duties, PID terms and
  sample age, `nullptr` when it is not running
- `waiting_reason(id)`: why an enabled thermostat does not run, the sentence its last failed
  start gave as a `warning`, at boot, a Save, an enable or when a relay it names came free
  (`not started: sensor 'temp_3' not found`, `not started: no free climate entity`); `""` once
  it runs or is disabled
- `claimed_by(relay_object_id)`: the id of the running thermostat holding it, or `""`;
  `holder_of(sw)`: its name, which `switch_hold::holder(sw)` answers with
- `sensor_reading(sensor_object_id)`: what a sensor reads now, `NaN` without a finite reading
  in °C
- `create(draft)`, `update(id, doc, revision)`, `remove(id)`, `set_enabled(id, enabled, take_over)`,
  `set_setpoint(id, value)`, `apply_preset(id, key)`, `start_autotune(id, direction, rule)`,
  `cancel_autotune(id)`: each returns a `Result` — `ok`, the HTTP
  `code` that fits (400, 404, 409, 413 for a file that would be over 8 KiB, 500, 507), an
  `error` sentence (the one the editor shows), the new `id`, the `holder` of a relay (running,
  or enabled and waiting), a `warning` when the thermostat was saved enabled but does not run
  (its sensor or a relay is not on the device, or no climate entity was free), `persisted`,
  false when the change is live but did not reach flash, the ids a take-over `stopped`, and the
  ids of the waiting thermostats that `started` on a relay the change freed. `apply_preset()`
  picks a preset by its key, running or not: 404 `Thermostat not found` or `Preset not found`
- a document's `from_newer_firmware()`: its file came from a newer firmware, so `update()`
  refuses it; `update()` refuses a `revision`, when one is given, that is not the stored one, with
  409 `The device changed this thermostat since it was read; reload it`
- `start_autotune()` with no direction takes the mode's, which `heat_cool` has none of; it is
  refused with 404, 409 (a bang-bang or stopped thermostat, a newer firmware's file, one
  calibrating already, mode off, a fault) or 400 (a direction the mode does not drive, or none in
  `heat_cool`). `cancel_autotune()` is 409 when nothing runs. `autotune(id)`: the last run since
  boot, `nullptr` for none
- `validate_name(name, &error)`, `is_name_taken(name, exclude_id, &error)`

## Testing

`python tests/run.py climate_hub` builds the component for the ESPHome `host` platform into a
Google Test binary and runs it, with a directory standing in for the flash and a clock the test
moves; the cases are in `tests/components/climate_hub/`. See
[doc/TESTING.md](../../doc/TESTING.md).
