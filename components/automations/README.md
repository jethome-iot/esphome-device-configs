# automations

Rules the device runs without a recompile. They are JSON files on a filesystem storage, loaded
at boot into entity pointers and driven by entity state callbacks, a one-second cron tick and a
startup event. Rules can be added, changed, enabled and removed while the device runs.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, automations, loop_job, switch_hold]

i2c:
  sda: 5
  scl: 4

time:
  - platform: pcf8563
    id: pcf8563_time

littlefs_storage:
  id: user_storage

automations:
  storage: user_storage
  time_id: pcf8563_time
```

## Options

| Option        | Default       | Meaning                                                                    |
| ------------- | ------------- | -------------------------------------------------------------------------- |
| `storage`     |               | A `filesystem_storage_abstract` backend, `littlefs_storage` on a device; required |
| `time_id`     |               | A `time` platform. Without one cron triggers are refused at boot, the rest still run |
| `folder_path` | `automations` | The folder below the backend's base path that holds the rule files         |

## A rule

```json
{
  "id": 1,
  "name": "Porch light",
  "enabled": true,
  "mode": "single",
  "triggers": [{"source": "input", "type": "click", "object_id": "input_1"}],
  "condition": {"type": "input", "object_id": "input_3", "state": "true"},
  "actions": [
    {"source": "switch", "type": "turn_on", "object_id": "relay_1"},
    {"source": "delay", "delay_ms": 5000},
    {"source": "switch", "type": "turn_off", "object_id": "relay_1"}
  ]
}
```

Entities are named by their object id. Any trigger fires the rule; the condition, when there is
one, picks `actions` or `else_actions`.

| Key                 | Values                                                                      |
| ------------------- | --------------------------------------------------------------------------- |
| `triggers[].source` | `input` (`press`, `release`, `click`, `state_change`), `switch` (`turn_on`, `turn_off`, `state_change`), `temperature` — any numeric sensor, despite the key (`below` / `above` with `threshold`, `range` with `min_threshold` and `max_threshold`, in the sensor's unit), `cron`, `startup` |
| `condition.type`    | `input` with `state`, `temperature` with `temperature_type` — any numeric sensor, as for the trigger (`below` / `above` with `threshold`, `range` with `min_threshold` and `max_threshold`), and `and` / `or` / `xor` over a `conditions` list, nested freely |
| `actions[].source`  | `switch` (`turn_on`, `turn_off`, `toggle`, `follow` with `invert`), `delay` with `delay_ms`, `climate` with `climate` (`turn_on`, `turn_off`, `set_preset` with `preset`, `set_target` with `target`, `follow` with `on` and `off`) |
| `mode`              | `single` ignores a trigger while the rule runs, `restart` starts over, `parallel` runs up to 8 copies |
| `enabled`           | `true` when absent; a disabled rule is loaded and listed but never fires |
| `cron_preset`       | The editor's own note about the form it offered: `daily`, `hourly`, `every_n_minutes`, `weekly`, `monthly`, `custom`. The engine never reads it, and writes it back only when the file had one |

A `click` is a press between 200 and 1000 ms. A `temperature` trigger fires on the crossing and
arms again when the value goes back. `above` and `below` are strict, a range includes both ends,
for triggers and conditions alike. `follow` drives its target from the state the trigger
carried; in `single` mode a trigger ignored while the rule runs still hands its state to a
`follow` that has not played yet. A `switch` action on a relay a running thermostat drives (a
[`switch_hold`](../switch_hold/switch_hold.h) holder) does nothing but log the rule and the
thermostat, and the run goes on. `cron` is six fields, seconds first — `"*/2 * * * * *"`,
`"0 30 6,18 1 * *"` — with `*`, `*/N`, `X-Y`, `X-Y/N` and lists; a field that matches nothing
is rejected.

A jump of the `time_id` clock back by more than 15 minutes is not handled: the moments it
passes again fire a second time.

## Thermostat actions

A `climate` action acts on a [`climate_hub`](../climate_hub/README.md) thermostat, running or
not, as Home Assistant would. A firmware without `climate_hub` has no thermostat to name, so a
rule with such an action is never built there.

```json
{"source": "climate", "type": "set_preset", "climate": "living-room", "preset": "eco"}
{"source": "climate", "type": "follow", "climate": "living-room",
 "on": {"type": "set_preset", "preset": "comfort"}, "off": {"type": "set_preset", "preset": "eco"}}
```

- `climate` is the thermostat's id, the name of its file, never its entity: renaming the
  thermostat leaves the rule working. `preset` is a preset's key.
- `turn_off` sets mode off. `turn_on` sets the mode it was in before it went off, which its file
  keeps across a reboot.
- `set_target` sets the target to `target`, in °C, held inside the thermostat's range.
- `follow` takes `on` while the state the trigger carried is on and `off` while it is off, each
  `{"type": "turn_on"}`, `{"type": "turn_off"}` or `{"type": "set_preset", "preset": "<key>"}`.
  A trigger that carries no state leaves the thermostat alone. A preset that keeps the mode does
  not turn an off thermostat on: for on and off plus a preset, use two actions.
- A thermostat moves on the next loop pass, not inside the callback that fired the rule; the
  actions after it in the run follow on that pass. A rule restarted or stopped before then does
  not move it.

## Storage

One file per rule, `<base_path>/<folder_path>/<name>.json`, the name lower-cased with runs of
spaces, dashes and underscores collapsed into one `_` and cut to 48 bytes. The name is the key
on disk, so two rules cannot share one.

The folder is meant to be writable by hand, so what it holds is repaired at boot: a rule in a
file named after something else is moved to its own file, a missing or repeated `id` is
restamped, and a second rule claiming a taken name becomes `<name> 2`.

A file that is empty, larger than 16 KiB, not valid JSON, or that spells any of the words above
in a way the engine does not know is refused whole and left exactly as it is: the log names the
word and the file. Nothing is loaded from it, so a rule with one typo never runs half of what it
says — and a repair never writes the engine's guess over what its author wrote.

A removal or a rename that the partition refuses to carry out leaves the file empty rather than
whole, so the rule does not come back at the next boot. The name it held stays taken until that
file is deleted by hand.

## Missing entities and thermostats

Every entity reference is the `fnv1_hash` of an object id, resolved once at boot. A rule naming
an entity that is not there stays on disk but is not built — the log says why and `dump_config`
marks it `(not built: <why>)`. Renaming an entity orphans the rules that used it.

Such a file is never rewritten, not even to restamp its `id` or move it to its own name: a file
holds the object ids, memory holds only their hashes, so a rewrite would blank the names. Put
the entity back and the next boot repairs the file as usual.

A rule naming a thermostat, or a preset key, that the hub does not have is refused when it is
added or changed, and at boot it is not built either, its file kept. Thermostats come and go
while the device runs, so the rule follows: it is built once the hub creates that thermostat or
a Save gives it that preset again, and drops out, file kept, when the thermostat is removed or
the preset goes. A rule that builds before and after keeps running. The thermostat's id is kept
as written, so such a file is repaired at boot like any other.

Why a rule is not built, in a sentence the editor shows, is its config's `build_error`:
`Trigger 1: input not found`, `Action 2: thermostat "attic" not found`,
`Else action 1: thermostat "living-room" has no preset "eco"`; `""` when it is built.

## From lambdas

`esphome::global_automation_storage` is the component. The mutators may be called from any task;
they run on the loop task and block the caller. From inside a rule's own action (a switch
callback, say) they refuse and return false.

- `add_automation(config, &error)`: the assigned id, `0` on failure; `error`, when given, says
  what the rule names that is missing, when that is why
- `update_automation(id, config, &error)`, `remove_automation(id)`
- `set_enable_automation(id, enable, persisted = nullptr)`: `persisted` says whether it reached flash
- `reset_all()`: remove every rule and its file
- `is_name_taken(name, exclude_id = 0)`: names collide by file name
- `configs()`: the loaded `AutomationConfig`s, including the ones that did not build, each with
  its `build_error`
- `run_on_loop(job)`: runs `job` on the loop task and blocks; `false` when the loop never got to it

`configs()` and `is_name_taken()` read the list where it lives, so they are for the loop task —
a mutator called from a lambda there reallocates it under a reader on any other. A caller that
is not the loop task reads through `run_on_loop`, together with whatever it decides from the
read, so that its answer describes one state of the list.

## Testing

`python tests/run.py automations` builds the engine for the ESPHome `host` platform into a Google
Test binary and runs it, with a directory standing in for the flash; the cases are in
`tests/components/automations/`. See [doc/TESTING.md](../../doc/TESTING.md).
