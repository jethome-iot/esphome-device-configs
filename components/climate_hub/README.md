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
    components: [filesystem_storage_abstract, littlefs_storage, climate_hub, loop_job]

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
  "version": 1,
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
  "setpoint": 21
}
```

| Key                  | Values                                                                     |
| -------------------- | -------------------------------------------------------------------------- |
| `id`                 | Made from the name when the thermostat is created (`a-z`, `0-9`, single dashes, at most 48; `New` gets `new-2`, since the dashboard opens a blank editor at `new`), then never changes; the file is `<id>.json` |
| `name`               | 1 to 48 printable ASCII characters, neither `/` nor `\`, trimmed; also the climate entity's name |
| `kind`               | `bang_bang` (the default) or `pid`                                         |
| `sensor_id`          | The object id of a temperature sensor that reports °C, `temp_1` for `Temp 1`; at most 120 characters, the longest an object id gets |
| `heat`, `cool`       | `relay_id`: the object id of a switch, `""` for a direction not used; at least one, not the same one twice, at most 120 characters |
| `mode`               | `off`, `heat`, `cool` or `heat_cool`; a mode needs the relays it drives    |
| `setpoint`           | The one target, held inside `visual.min_temperature` … `visual.max_temperature` |
| `bang_bang`          | The switching points sit `below` and `above` the target                   |

Every number is clamped into its range, and a missing one takes its default: the ranges and the
defaults are the table in `param_table.cpp`. A document built in C++ and handed to `create()` or
`update()` is clamped the same way. A document that breaks a rule above is refused
whole with a sentence that says which. A thermostat that is to run is created, saved or enabled
only when its sensor, if it is on the device, reports °C, and no running thermostat holds its
relays. One whose sensor or a relay is not on the device is saved or enabled all the same, and
waits as it would at boot, with a `warning` that names what is missing. A disabled one may name
what is not there yet.

## Control

- **Hysteresis.** Heating starts below `setpoint - below` and stops above `setpoint + above`;
  cooling the other way round. Between the two points the relay keeps what it was doing.
- **PID.** ESPHome's `pid` law, run every `update_interval_s`, drives each relay as a slow PWM
  over `period_s`: positive output heats, negative cools.
- A change of mode or target takes effect at the next loop pass, not at the next interval.
- **Safety.** A thermostat keeps its relays open until its first reading, idle and without a
  fault; with none within `sensor_timeout_s` of its start it reports `sensor_stale`. It opens
  them as well when its sensor has been silent for `sensor_timeout_s`, while the reading is
  above `safety.max_temperature`, and in mode `off`. A thermostat that starts shows its
  sensor's last value at once, but acts on it only if it arrived within `sensor_timeout_s`
  while a thermostat was running on that sensor, and the timeout runs from that reading;
  otherwise it waits for the next one. A reading that is not a finite number, `NaN` or an
  infinity, is no reading: it is neither shown nor acted on, so a sensor that sends nothing
  else reports `sensor_stale` once `sensor_timeout_s` has passed since its last good one.
- `min_on_s` and `min_off_s` hold a relay closed or open that long after it moved, whichever
  thermostat moved it or put it back; a safety cut-out does not wait for them. Keeping an open
  relay open, as while a thermostat waits for its first reading, is no move.
- A Save keeps what the thermostat is doing: inside the band a hysteresis thermostat goes on
  heating, cooling or idling as it was in the modes it still has, and the PWM keeps its rhythm
  unless `period_s` changes. Unless it changes `kind` or `sensor_id`, it also keeps what a PID
  has learnt: new gains apply from the next pass, and the integral is clamped into new limits.
  `starting_integral_term` applies when a thermostat starts and after a Save that changes
  either. A Save that keeps `sensor_id` does not restart the wait for a first reading.
- The entity reports the room temperature to a tenth of a degree, the target in steps of
  `visual.step`, the mode and what it is doing: heating, cooling, idle, or off, which only
  mode `off`, a fault or a stopped thermostat shows. Home Assistant and the web server can set
  the mode and the target; a target outside the range is clamped to it.

## Relays

A running thermostat holds its relays, and puts one back within a loop pass if anything else
moves it — from the panel, over Modbus, from an automation or from Home Assistant. Two
thermostats may name the same relay and take turns: only one of them can run at a time.
Starting the second while the first runs is refused, naming the one that holds it, unless it
takes the relay over, which stops the holder. A take-over by one whose sensor or a relay is not
on the device is refused, and the holder runs on. Stopping a thermostat opens its relays. A Save
that keeps a relay leaves it where it is, and so does a take-over: a relay both thermostats
drive changes hands as it is, and the holder's other relays open.

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
renamed, gains or loses its heating or cooling relay, or gets a new temperature range or step:
the whole device is unavailable there for about five seconds. A burst of such edits costs one
reconnect. Swapping one relay for another, a new target, mode, band or gains, and any change to
a thermostat that is not running cost none.

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
  whose relay another one holds, stays enabled and does not run.

A thermostat that waits is not started when what it names turns up later, only by the next boot
that finds it, or by a Save or an enable once it is there.

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
- `claimed_by(relay_object_id)`: the id of the running thermostat holding it, or `""`
- `sensor_reading(sensor_object_id)`: what a sensor reads now, `NaN` without a finite reading
  in °C
- `create(draft)`, `update(id, doc)`, `remove(id)`, `set_enabled(id, enabled, take_over)`,
  `set_setpoint(id, value)`: each returns a `Result` — `ok`, the HTTP `code` that fits (400,
  404, 409, 413 for a file that would be over 8 KiB, 500, 507), an `error` sentence (the one
  the editor shows), the new `id`, the `holder` of a relay, a `warning` when the thermostat was
  saved enabled but does not run (its sensor or a relay is not on the device, or no climate
  entity was free), and `persisted`, false when the change is
  live but did not reach flash
- `validate_name(name, &error)`, `is_name_taken(name, exclude_id, &error)`

## Testing

`python tests/run.py climate_hub` builds the component for the ESPHome `host` platform into a
Google Test binary and runs it, with a directory standing in for the flash and a clock the test
moves; the cases are in `tests/components/climate_hub/`. See
[doc/TESTING.md](../../doc/TESTING.md).
