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
| Relay | `Bind to` | `None`, `Input 1` … `Input 6`, the virtual inputs | The input that drives this relay directly; needs a `Binding` other than `Disabled` |
| Relay | `Binding` | `Disabled`, `Toggle`, `Follow` | `Toggle` flips the relay on each rising edge of the input as reported, after its `Inverted`; `Follow` makes the relay copy the input, at boot too, so it wins over `Start mode` |
| Input | `Inverted` | `No`, `Yes` | A closed contact is reported as Off. Flipping it re-reports the input at once; bindings and automations take that as a level, not as a press or release |
| Relay, Input | `Label` | Text, up to 24 characters; empty for none | Shown on the panel and the dashboard in place of the name. Set from the dashboard only; it changes nothing else, so Home Assistant, the REST routes, Modbus, automations and thermostats still see the name. A temperature slot's [label](ONEWIRE_WORKFLOW.md#label) takes the same text |

The binding rules are in [components/bindings](../components/bindings/README.md). A binding
does not move a relay a running thermostat drives; once the thermostat frees it, a `Follow`
relay takes its input's state at once. What else such a relay does and does not do is in
[Thermostats](CLIMATE.md#a-running-thermostats-relays).

## Display menu

**Relays → Relay N** holds `State`, `Inverted`, `Start mode`, `Bind to` and `Binding`;
**Inputs → Input N** holds the live state and `Inverted`. Both have a first row naming the
relay or input, by its label or by its name when it has none, cut to 18 characters, and open
on the row below it. CENTER opens a setting, LEFT and RIGHT step through its choices, CENTER or BACK
closes it; the choice is applied when the row closes and written to the partition a few
seconds later. On a relay a running thermostat drives, `State` does not toggle and an
`Inverted` change is refused; the log names the thermostat.

A relay or an input with a label goes by it in the Relays and Inputs lists and in `Bind to`'s
choices, as soon as it is set. A label too long for its row is cut, to 12 characters in the
lists, where the state follows it, and to 8 in `Bind to`, the last of them `…`. A character the
menu font cannot draw shows as `?`; the font has Latin and Cyrillic.

## Virtual inputs

An input set up on the dashboard, under **Settings → Inputs**, that follows one physical input
through an on delay and an off delay. A motion sensor that drops a few seconds after the last
movement becomes "someone is here" that holds for ten minutes after the last pulse, so a
thermostat following it does not flip on every pause, and a relay bound to it is a staircase
light with no rule.

| Setting | Choices | Effect |
| --- | --- | --- |
| `Source` | `Input 1` … `Input 6` | The physical input it follows, never another virtual input or a relay |
| `On delay` | 0 to 86400 s | It turns on once the source has stayed on that long |
| `Off delay` | 0 to 86400 s | It turns off that long after the source last went off; every new pulse starts the count again |
| `Label` | As for an input | Shown on the panel and the dashboard in place of the name |

It is an input wherever one is taken: a rule's trigger and condition, a relay's `Bind to` on the
panel and the dashboard, a `binary_sensor` in Home Assistant and a row of the panel's Inputs list.
It goes by `Virtual input N` (object id `virtual_input_N`) whatever its label, so rules and
bindings keep pointing at it. It has no `Inverted` and no Modbus register. The firmware has 16
(`max_inputs` in `devices/JXD/packages/features/virtual-inputs.yaml`, up to 32).

A change is in force at once, its delays counted from the save; at boot a virtual input takes its
source's state as soon as the source reports, the on delay counting from then. One whose source
this firmware does not have stays off, and the dashboard says why. A new one or a deleted one
makes Home Assistant reconnect to list it or drop it. A deleted one takes its label with it, and
every relay bound to it goes to `Bind to: None`, staying where it is; a rule that names it finds no
input, as for any input this firmware lacks. The dashboard's Delete names those rules and relays
first. A new one takes the lowest free slot that no rule and no binding names, so nothing set up
for a deleted one follows it; only when every free slot is named does it take the lowest free one.

The panel lists, after the physical inputs, the virtual inputs there were at boot, each with its
label or name and its state, read only: one added later shows after a reboot, and one deleted
shows `--` until then.

They are kept in `/littlefs/config/virtual_inputs.json`, next to the files below, so an update
keeps them, a factory reset clears them, and a backup of the partition carries them. The
dashboard's Restore puts them back at once, before the rules that name them; their labels come
back with the other input settings at the next boot. The format is in
[components/virtual_inputs](../components/virtual_inputs/README.md).

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
menu, the FN button or the API — formats the partition, so these files go with it. A firmware
older than labels keeps the files but drops the labels the first time it saves one of them.

## Configuration

`devices/JXD/packages/features/entity-settings.yaml`; the JSON store itself is configured in
`storage.yaml` next to the partition. The components are documented in
[components/config_json](../components/config_json/README.md) and
[components/entity_config](../components/entity_config/README.md) and
[components/bindings](../components/bindings/README.md).
