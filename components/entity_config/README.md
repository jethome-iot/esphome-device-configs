# entity_config

Settings types for the relays and inputs of a JXD controller, stored by
[`config_json`](../config_json/README.md) and applied at boot before the entities set themselves
up. Each type is opt-in through `settings:` and costs nothing when it is off.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [config_base, config_json, entity_config, panel_text, switch_hold]

config_json:
  id: config_json_keeper
  storage: user_storage

entity_config:
  settings: [switch, binary_sensor]
  switch_settings_id: switch_settings
  binary_sensor_settings_id: binary_sensor_settings
```

## Options

| Option                      | Default                  | Meaning                                                    |
| --------------------------- | ------------------------ | ---------------------------------------------------------- |
| `config_json_id`            | the single `config_json:` | The keeper that loads and saves the types                  |
| `settings`                  | `[switch, binary_sensor]` | Which types to enable; each needs its `switch:` / `binary_sensor:` section |
| `<type>_settings_id`        | generated                | Names the settings object so lambdas can address it        |
| `<type>_apply_id`           | generated                | Names the component that applies that type at boot         |

## Types

| Type            | Record key | Fields | Applied |
| --------------- | ---------- | ------ | ------- |
| `switch`        | object_id  | `restore_mode`, `inverted`, `binding_input`, `binding_mode`, `label`; a field a record leaves out, or names wrongly, keeps the switch's compiled value | At `HARDWARE + 1`, before the switch's own setup. A `RESTORE_*` mode chosen at run time stores the current state at once, since the switch itself makes its preference only in setup. |
| `binary_sensor` | object_id  | `inverted`, `label` | A filter appended to the end of the sensor's chain; a flip re-emits the last raw level, so it shows without waiting for an edge. |

`binding_input` and `binding_mode` round-trip as plain strings; they do nothing without the
[`bindings`](../bindings/README.md) component, and the lambdas that edit them are compiled out
with it.

`inverted` does not change on a relay a running thermostat drives (a
[`switch_hold`](../switch_hold/switch_hold.h) holder): the dashboard's write is refused with the
thermostat's name, and `set_option` refuses the menu's. The relay's other fields change as usual.

## Labels

`label` is what the panel and the dashboard show in place of the entity's name; empty or absent
means none, and the name shows. The name, the object id and everything keyed by them —
Home Assistant, `web_server`'s routes, Modbus, rules, thermostats, these records — stay as they
are. A label is valid UTF-8 with no control character and at most 24 characters (code points,
not bytes); the spaces at both ends are trimmed. The rules are
[`panel_text`](../panel_text/README.md#labels)'s, which a temperature slot's label in
[`dallas_scan`](../dallas_scan/README.md#labels) follows too. A write with anything else is
refused, and a label in the file that breaks the rules is dropped with a warning. It is set through
`web_device_dashboard`; a write that leaves it out, or sends `null`, keeps it, on both types. It
applies at once.

## Naming the settings objects

A `<type>_settings_id` that is left out is generated, and a generated id cannot be named from a
lambda: `id(switch_settings)` then fails validation with "Couldn't find ID". Set the ids for
every type a menu or an automation addresses. The ids are declared for both types, but the
object only exists for the enabled ones — naming a disabled type's id passes validation and
fails at link.

## From lambdas

`switch` settings, given a `switch_::Switch *` and a `Field` (`INVERTED`, `RESTORE_MODE`, and
with the `bindings` component `BINDING_INPUT`, `BINDING_MODE`), each field is a list of options:

- `option_count(field)`, `option_label(sw, field, index)`
- `option_index(sw, field)`: the stored value's index, `-1` when the list does not offer it (a
  hand-edited file); `option_label` with `-1` names that value
- `set_option(sw, field, index)`: `false`, nothing changed, for an index out of range or an
  `INVERTED` the relay's thermostat holds it at; `inverted_refusal(sw)` is the sentence that says
  so, `""` when nothing holds the relay. It names the relay by its name; the dashboard's refusal
  names it by `display_name()`

`binary_sensor` settings, given a `binary_sensor::BinarySensor *`: `is_inverted(sensor)`,
`set_inverted(sensor, inverted)`.

A set applies at once and schedules the debounced save.

Both: `display_name(entity)` is what to show for the entity, its label or its name when it has
none, read live. With both types enabled, `Bind to`'s options name the inputs the same way.
