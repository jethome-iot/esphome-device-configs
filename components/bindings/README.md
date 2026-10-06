# bindings

An input drives an output, with no automation in between. A binding is two fields on the
per-switch record that [`entity_config`](../entity_config/README.md) keeps (`binding_input`,
`binding_mode`), so it shares that record's file and its display menu rows.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [config_base, config_json, entity_config, bindings, switch_hold]

bindings:
```

Nothing to configure; needs at least one `switch` and one `binary_sensor` in the configuration.

| | |
|---|---|
| Source | a `binary_sensor` |
| Target | a `switch` |
| Per output | at most one binding (a second write overwrites) |
| Per input | any number of outputs |

| `binding_mode` | Behaviour |
|---|---|
| `none` | No binding. |
| `toggle` | The output flips on the input's rising edge. |
| `follow` | The output mirrors the input. At boot it is driven once after every entity is up, so it wins over the switch's `restore_mode`. |

An output a running thermostat drives (a [`switch_hold`](../switch_hold/switch_hold.h) holder) is
left alone, at boot too, and the log says which thermostat has it. Once the thermostat lets go
of it, a `follow` output takes the input's state at once; a `toggle` one waits for the next edge.

Bindings do not chain: a switch is never a source. Conditions, delays and anything time-based
belong to automations.

## Setting one

From the display menu: **Relays → Relay N → Bind to** picks the input, **Binding** the mode.
In the record, `binding_input` is the input's object_id; an empty string or mode `none`
unbinds. A stored input that this build lacks is kept and logged, not dropped.

## From lambdas

`set_binding(output_key, input_key, mode)` and `remove_binding(output_key)` on
`bindings::global_bindings_manager`, keyed by `fnv1_hash(object_id)`. `entity_config` calls them
when a record is applied; there is no need to call them by hand.
