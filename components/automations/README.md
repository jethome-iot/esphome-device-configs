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
    components: [filesystem_storage_abstract, littlefs_storage, automations]

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
| `triggers[].source` | `input` (`press`, `release`, `click`, `state_change`), `switch` (`turn_on`, `turn_off`, `state_change`), `temperature` (`below` / `above` with `threshold`, `range` with `min_threshold` and `max_threshold`), `cron`, `startup` |
| `condition.type`    | `input` with `state`, `temperature` with `temperature_type` (`below` / `above` with `threshold`, `range` with `min_threshold` and `max_threshold`), and `and` / `or` / `xor` over a `conditions` list, nested freely |
| `actions[].source`  | `switch` (`turn_on`, `turn_off`, `toggle`, `follow` with `invert`), `delay` with `delay_ms` |
| `mode`              | `single` ignores a trigger while the rule runs, `restart` starts over, `parallel` runs up to 8 copies |
| `enabled`           | `true` when absent; a disabled rule is loaded and listed but never fires |
| `cron_preset`       | The editor's own note about the form it offered: `daily`, `hourly`, `every_n_minutes`, `weekly`, `monthly`, `custom`. The engine never reads it, and writes it back only when the file had one |

A `click` is a press between 200 and 1000 ms. A temperature trigger fires on the crossing and
arms again when the value goes back. `follow` drives its target from the state the trigger
carried. `cron` is six fields, seconds first — `"*/2 * * * * *"`, `"0 30 6,18 1 * *"` — with
`*`, `*/N`, `X-Y`, `X-Y/N` and lists; a field that matches nothing is rejected.

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

## Missing entities

Every entity reference is the `fnv1_hash` of an object id, resolved once at boot. A rule naming
an entity that is not there stays on disk but is not built — the log says why and `dump_config`
marks it `(not built)`. Renaming an entity orphans the rules that used it.

Such a file is never rewritten, not even to restamp its `id` or move it to its own name: a file
holds the object ids, memory holds only their hashes, so a rewrite would blank the names. Put
the entity back and the next boot repairs the file as usual.

## From lambdas

`esphome::global_automation_storage` is the component. The mutators may be called from any task;
they run on the loop task and block the caller.

- `add_automation(config)`: the assigned id, `0` on failure
- `update_automation(id, config)`, `remove_automation(id)`
- `set_enable_automation(id, enable, persisted = nullptr)`: `persisted` says whether it reached flash
- `reset_all()`: remove every rule and its file
- `is_name_taken(name, exclude_id = 0)`: names collide by file name
- `configs()`: the loaded `AutomationConfig`s, including the ones that did not build

## Testing

`python tests/unit/run.py` builds the engine for the ESPHome `host` platform into a Google Test
binary and runs it, with a directory standing in for the flash. See [doc/TESTING.md](../../doc/TESTING.md).
