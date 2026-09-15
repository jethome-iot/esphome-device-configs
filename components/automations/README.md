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
    components: [automations]

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
  "triggers": [{"source": "input", "type": "click", "object_id": "in_1"}],
  "condition": {"type": "input", "object_id": "in_3", "state": "true"},
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
| `condition.type`    | `input` with `state`, `temperature`, and `and` / `or` / `xor` over a `conditions` list, nested freely |
| `actions[].source`  | `switch` (`turn_on`, `turn_off`, `toggle`, `follow` with `invert`), `delay` with `delay_ms` |
| `mode`              | `single` ignores a trigger while the rule runs, `restart` starts over, `parallel` runs up to 8 copies |

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
restamped, and a second rule claiming a taken name becomes `<name> 2`. A file that is empty,
larger than 16 KiB or not valid JSON is skipped and left where it is.

## Missing entities

Every entity reference is the `fnv1_hash` of an object id, resolved once at boot. A rule naming
an entity that is not there stays on disk but is not built — the log says why and `dump_config`
marks it `(not built)`. Renaming an entity orphans the rules that used it.

## From lambdas

`esphome::global_automation_storage` is the component. The mutators may be called from any task;
they run on the loop task and block the caller.

- `add_automation(config)`: the assigned id, `0` on failure
- `update_automation(id, config)`, `remove_automation(id)`, `set_enable_automation(id, enable)`
- `reset_all()`: remove every rule and its file
- `is_name_taken(name, exclude_id = 0)`: names collide by file name
- `configs()`: the loaded `AutomationConfig`s, including the ones that did not build

## Testing

`python tests/host/run.py` builds the engine for the ESPHome `host` platform and runs it against
the rules in `tests/host/fixtures/`, with a directory standing in for the flash.
