# Entity settings

Per-relay and per-input settings that survive a reboot without a recompile, including an input
bound straight to a relay and a label of the owner's own. They are edited from the display menu
and the dashboard and kept as JSON files under `/littlefs/config/` on the user storage partition,
loaded at boot and applied before the entities set themselves up.

## What is settable

| Entity | Setting | Choices | Effect |
| --- | --- | --- | --- |
| Relay | `Inverted` | `No`, `Yes` | Swaps the physical output: the app's On drives the pin low. Refused while a running thermostat drives the relay |
| Relay | `Start mode` | `Off`, `On`, `Last` | What the relay does at power-up. On a relay a thermostat drives, `On`, or `Last` when it was on, closes it until the thermostat takes it over |
| Relay | `Bind to` | `None`, `Input 1` … `Input 6` | The input that drives this relay directly; needs a `Binding` other than `Disabled` |
| Relay | `Binding` | `Disabled`, `Toggle`, `Follow` | `Toggle` flips the relay on each rising edge of the input as reported, after its `Inverted`; `Follow` makes the relay copy the input, at boot too, so it wins over `Start mode` |
| Input | `Inverted` | `No`, `Yes` | A closed contact is reported as Off. Flipping it re-reports the input at once; bindings and automations take that as a level, not as a press or release |
| Relay, Input | `Label` | Text, up to 24 characters; empty for none | Shown on the panel and the dashboard in place of the name. Set from the dashboard only; it changes nothing else, so Home Assistant, the REST routes, Modbus, automations and thermostats still see the name |

The binding rules are in [components/bindings](../components/bindings/README.md). A binding
does not move a relay a running thermostat drives; once the thermostat frees it, a `Follow`
relay takes its input's state at once. What else such a relay does and does not do is in
[Thermostats](CLIMATE.md#a-running-thermostats-relays).

## Display menu

**Relays → Relay N** holds `State`, `Inverted`, `Start mode`, `Bind to` and `Binding`;
**Inputs → Input N** holds the live state and `Inverted`. CENTER opens a setting, LEFT and
RIGHT step through its choices, CENTER or BACK closes it; the choice is applied when the row
closes and written to the partition a few seconds later. On a relay a running thermostat
drives, `State` does not toggle and an `Inverted` change is refused; the log names the
thermostat.

A relay or an input with a label goes by it in the Relays and Inputs lists and in `Bind to`'s
choices, as soon as it is set. A label too long for its row is cut, to 12 characters in the
lists, where the state follows it, and to 8 in `Bind to`, the last of them `…`. A character the
menu font cannot draw shows as `?`; the font has Latin and Cyrillic.

## Files

`/littlefs/config/switch.json` and `/littlefs/config/binary_sensor.json`, one record per entity
keyed by its object_id:

```json
{"version": 1, "records": [{"source_name": "relay_1", "restore_mode": "ALWAYS_ON", "inverted": false,
                            "binding_input": "input_1", "binding_mode": "toggle",
                            "label": "Kitchen light"}]}
```

They can be edited by hand on the partition; the format and what happens to a damaged file are
in [components/config_json](../components/config_json/README.md). A factory reset — from the
menu, the FN button or the API — formats the partition, so these files go with it.

## Configuration

`devices/JXD/packages/features/entity-settings.yaml`; the JSON store itself is configured in
`storage.yaml` next to the partition. The components are documented in
[components/config_json](../components/config_json/README.md) and
[components/entity_config](../components/entity_config/README.md) and
[components/bindings](../components/bindings/README.md).
