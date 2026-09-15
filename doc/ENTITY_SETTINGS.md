# Entity settings

Per-relay and per-input settings that survive a reboot without a recompile. They are edited from
the display menu and kept as JSON files under `/littlefs/config/` on the user storage partition,
loaded at boot and applied before the entities set themselves up.

## What is settable

| Entity | Setting | Choices | Effect |
| --- | --- | --- | --- |
| Relay | `Inverted` | `No`, `Yes` | Swaps the physical output: the app's On drives the pin low |
| Relay | `Start mode` | `Off`, `On`, `Last` | What the relay does at power-up |
| Input | `Inverted` | `No`, `Yes` | A closed contact is reported as Off |

`Start mode: Last` starts saving the relay's state from the next boot: the switch decides whether
to keep a stored state when it sets itself up, one boot before the new mode is in place.

## Display menu

**Relays → Relay N** holds `State`, `Inverted` and `Start mode`; **Inputs → Input N** holds the
live state and `Inverted`. LEFT and RIGHT step through the choices; each step applies at once and
is written to the partition a few seconds later.

## Files

`/littlefs/config/switch.json` and `/littlefs/config/binary_sensor.json`, one record per entity
keyed by its object_id:

```json
{"version": 1, "records": [{"source_name": "relay_1", "restore_mode": "ALWAYS_ON", "inverted": false}]}
```

They can be edited by hand on the partition. A record naming an entity this firmware does not
have is logged and skipped; a file that is missing or unreadable leaves the entities at their
compiled defaults and is left alone rather than rewritten. A factory reset from the menu clears
the preferences, not these files.

## Configuration

`devices/JXD/packages/features/entity-settings.yaml`. The components are documented in
[components/config_json](../components/config_json/README.md) and
[components/jxd_config](../components/jxd_config/README.md); `jxd_config` also carries timezone,
uart, mqtt and auth settings, which this firmware leaves off.
