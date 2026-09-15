# jxd_config

Settings types for the entities of a JXD controller, stored by [`config_json`](../config_json/README.md)
and applied at boot before the entities set themselves up. Each type is opt-in through
`settings:` and costs nothing when it is off.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [config_base, config_json, config_nvs, uart_list, jxd_config]

config_json:
  id: config_json_keeper
  storage: user_storage

jxd_config:
  settings: [switch, binary_sensor]
  switch_settings_id: switch_settings
  binary_sensor_settings_id: binary_sensor_settings
```

`config_nvs` and `uart_list` are named because `jxd_config` imports them, not because they are
used: neither has to be configured, and an unconfigured one contributes no code to the firmware.

## Options

| Option                      | Default                  | Meaning                                                    |
| --------------------------- | ------------------------ | ---------------------------------------------------------- |
| `config_json_id`            | the single `config_json:` | The keeper that loads and saves the JSON types             |
| `settings`                  | `[switch, binary_sensor]` | Which types to enable                                      |
| `<type>_settings_id`        | generated                | Names the settings object so lambdas can address it        |
| `<type>_apply_id`           | generated                | Names the component that applies that type at boot         |
| `uart_list_id`              |                          | Required by `uart`                                         |
| `time_ids`                  |                          | Required by `timezone`                                     |
| `config_nvs_id`             |                          | Required by `auth`                                         |

## Types

| Type            | Record key | Fields | Applied |
| --------------- | ---------- | ------ | ------- |
| `switch`        | object_id  | `restore_mode`, `inverted`, `binding_input`, `binding_mode` | At `HARDWARE + 1`, before the switch's own setup. A `RESTORE_*` mode chosen at run time starts persisting from the next boot, because the switch creates its preference in setup. |
| `binary_sensor` | object_id  | `inverted` | A filter appended to the end of the sensor's chain; a flip re-emits the last raw level, so it shows without waiting for an edge. |
| `uart`          | index in `uart_list` | `baud_rate`, `parity`, `stop_bits` | Setters plus `load_settings(false)`. |
| `timezone`      | –          | `timezone` | Pushed to every `time_ids` entry. |
| `mqtt`          | –          | broker, credentials, prefix, discovery | Client reconfigured and reconnected; needs `mqtt:`. |
| `auth`          | –          | `username`, `password` | Web-server credentials in NVS; needs `config_nvs:` and `web_server: auth:`. |

Only `switch` and `binary_sensor` are enabled on the JXD-R6-E1ETH-LCD. The other four ship as
code and are left off: `timezone` would fight the stored timezone that `features/rtc-time.yaml`
restores at `on_boot` priority 600, `uart` covers `jxm_uart1` while `jxm_uart2` belongs to the
Modbus server, which pushes its own settings at priority 700, `mqtt` needs an `mqtt:` section
and `auth` a `web_server: auth:` block — this firmware has neither.

`binding_input` and `binding_mode` round-trip as plain strings; they do nothing without the
`bindings` component, and the lambdas that edit them are compiled out with it.

## Naming the settings objects

A `<type>_settings_id` that is left out is generated, and a generated id cannot be named from a
lambda: `id(switch_settings)` then fails validation with "Couldn't find ID". Set the ids for
every type a menu or an automation addresses. The ids are declared for all six types, but the
object only exists for the enabled ones — naming a disabled type's id passes validation and
fails at link.

## From lambdas

`switch` settings, given a `switch_::Switch *`:

- `inverted_label(sw)`, `toggle_inverted(sw)`
- `restore_mode_label(sw)`, `cycle_restore_mode(sw, ±1)`
- `binding_input_label(sw)`, `cycle_binding_input(sw, ±1)`, `binding_mode_label(sw)`,
  `cycle_binding_mode(sw, ±1)` — only with the `bindings` component

`binary_sensor` settings, given a `binary_sensor::BinarySensor *`:

- `inverted_label(sensor)`, `is_inverted(sensor)`, `toggle_inverted(sensor)`

Each edit applies at once and schedules the debounced save. The cycles wrap around, and from a
value the list does not offer — a hand-edited file — the next step lands on the first option.
