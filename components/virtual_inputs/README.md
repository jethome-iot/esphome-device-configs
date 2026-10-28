# virtual_inputs

Binary sensors that follow a physical input through an on delay and an off delay, set up at run
time rather than declared in YAML. A motion sensor that drops a few seconds after the last
movement becomes "someone is here" that holds for ten minutes after the last pulse, and a relay
bound to it is a staircase light with no rule at all.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, config_base, config_json, virtual_inputs]

config_json:
  id: config_json_keeper
  storage: user_storage

virtual_inputs:
  id: virtual_input_pool
  max_inputs: 16
  web_server:
    sorting_group_id: group_inputs
    sorting_weight: 7
```

## Options

| Option           | Default | Meaning                                                                         |
| ---------------- | ------- | ------------------------------------------------------------------------------- |
| `max_inputs`     | `16`    | Slots, 1-32                                                                     |
| `config_json_id` |         | The [`config_json`](../config_json/README.md) keeper whose folder holds the file; may be left out with a single `config_json:` |
| `web_server`     |         | `sorting_group_id` and `sorting_weight`; slot N gets weight + N - 1             |

The id cannot be `virtual_inputs`, the component's own name. The build refuses
`web_server: include_internal: true`, which would list the free slots, and a YAML
`binary_sensor` whose name gives a slot's object id (`Virtual input 3`).

## Slots

Slot N is a `binary_sensor` named `Virtual input N`, object id `virtual_input_N`, for good: a
label changes neither, so rules, bindings and Home Assistant keep finding it. A free slot is
internal, so nothing outside the firmware sees it. A slot in use follows one source:

- on once the source has stayed on for `on_delay` seconds;
- off once `off_delay` seconds have passed since the source last went off; a pulse while that
  counts starts it again;
- either delay may be 0, and neither more than 86400 (a day).

The source is a physical input: a visible `binary_sensor` that is not a virtual input, so a
virtual input never follows another and nothing loops through one. It follows the source as
the source reports, its `Inverted` included. At boot a slot has no state until its source
reports, then takes that level, the on delay counting from then. A save puts the slot in force
at once, its delays counted again from the save. A slot whose source this firmware does not
have — the file edited by hand, a build without that input — stays off, and `problem()` says
why.

A slot set up or deleted is an entity that came or went, which Home Assistant learns only when
it connects: the component asks it to reconnect two seconds later, one reconnect for a burst of
changes.

## Labels

With [`entity_config`](../entity_config/README.md)'s `binary_sensor` type in the firmware, a
slot's label is that type's record for `virtual_input_N`, kept and shown like a physical input's
and set the same way. A slot takes no inversion there: a write that sets `inverted` is refused,
and one stored in the file is ignored with a warning. A delete drops the slot's record, so the
next virtual input in that slot starts without the old label.

## Storage

`<config_dir>/virtual_inputs.json` in the keeper's folder:

```json
{"version": 1, "records": [{"slot": 1, "source": "input_3", "on_delay": 0, "off_delay": 600}]}
```

The file is read at boot only. It can be edited or restored by hand, then the device rebooted.
A record needs a `slot` from 1 to `max_inputs` and a `source`, an object id; `on_delay` and
`off_delay` are whole seconds from 0 to 86400, 0 when left out. A record that breaks a rule is
skipped and the rest load; one with a slot past `max_inputs` stays in the file until the next
write, so lowering `max_inputs` does not lose it at once; of two records for one slot the later
wins. A file that is there but does not load leaves every slot free for that boot and is not
written over: saves and deletes are refused until it is fixed and the device rebooted. Without
the partition there are no virtual inputs and nothing is saved.

The file lives on the user partition, so an OTA update keeps it, a factory reset wipes it, and a
backup of the partition carries it.

## From lambdas

Slots are 0-based here. Everything runs on the loop task.

- `max_inputs()`
- `configured(slot)`, `config(slot)`: `source`, `on_delay` and `off_delay` in seconds; an empty
  source for a free slot
- `sensor(slot)`: the slot's binary sensor, in use or not; `nullptr` past the pool
- `is_virtual_input(sensor)`, `first_free()` (`-1` when every slot is in use)
- `problem(slot)`: why a slot in use is off whatever its source does or has no state yet, `""`
  while it follows its source
- `check_source(object_id)`: `OK`, `MISSING` or `VIRTUAL`; `sources()`, what a slot may follow
- `can_save()`: the keeper's folder can be written; `file_loaded()`: the file loaded at boot or
  there was none
- `save(slot, config)`: write and put in force; false, with nothing changed, for a slot past the
  pool, a source that is not `OK`, a delay past 86400, storage that cannot keep it or a write
  that fails
- `remove(slot)`: write, hide and drop the label; false, with nothing changed, for a free slot or
  as `save()`

[`web_device_dashboard`](../web_device_dashboard/README.md) lists, saves and deletes them over
HTTP; its page does under **Settings → Inputs**.

## Tests

`tests/components/virtual_inputs/` covers the delays, the file, the pool and its boot, and a slot
as a binding's source, a rule's trigger and condition and an `entity_config` label, booted in the
firmware's priority order. The real LittleFS and the Home Assistant reconnect are out of reach on
the host: a directory stands in for the partition, and the reconnect is counted.
