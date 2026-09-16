# Entity settings

Per-relay and per-input settings that survive a reboot without a recompile, including an input
bound straight to a relay. They are edited from the display menu and kept as JSON files under
`/littlefs/config/` on the user storage partition, loaded at boot and applied before the entities
set themselves up.

## What is settable

| Entity | Setting | Choices | Effect |
| --- | --- | --- | --- |
| Relay | `Inverted` | `No`, `Yes` | Swaps the physical output: the app's On drives the pin low |
| Relay | `Start mode` | `Off`, `On`, `Last` | What the relay does at power-up |
| Relay | `Bind to` | `None`, `Input 1` … `Input 6` | The input that drives this relay directly; needs a `Binding` other than `Disabled` |
| Relay | `Binding` | `Disabled`, `Toggle`, `Follow` | `Toggle` flips the relay on each press of the input; `Follow` makes the relay copy the input, at boot too, so it wins over `Start mode` |
| Input | `Inverted` | `No`, `Yes` | A closed contact is reported as Off; bindings see the input after this |

The binding rules are in [components/bindings](../components/bindings/README.md).

## Display menu

**Relays → Relay N** holds `State`, `Inverted`, `Start mode`, `Bind to` and `Binding`;
**Inputs → Input N** holds the live state and `Inverted`. CENTER opens a setting, LEFT and
RIGHT step through its choices, CENTER or BACK closes it; the choice is applied when the row
closes and written to the partition a few seconds later.

## Files

`/littlefs/config/switch.json` and `/littlefs/config/binary_sensor.json`, one record per entity
keyed by its object_id:

```json
{"version": 1, "records": [{"source_name": "relay_1", "restore_mode": "ALWAYS_ON", "inverted": false,
                            "binding_input": "input_1", "binding_mode": "toggle"}]}
```

They can be edited by hand on the partition; the format and what happens to a damaged file are
in [components/config_json](../components/config_json/README.md). A factory reset from the menu
clears the preferences, not these files.

## Configuration

`devices/JXD/packages/features/entity-settings.yaml`; the JSON store itself is configured in
`storage.yaml` next to the partition. The components are documented in
[components/config_json](../components/config_json/README.md) and
[components/entity_config](../components/entity_config/README.md) and
[components/bindings](../components/bindings/README.md).
