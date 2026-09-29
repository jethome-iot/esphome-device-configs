# Thermostats

Thermostats the device runs without a recompile. Each one reads a temperature sensor — a
`Temp N` probe, say — and switches a heating relay, a cooling relay or both, with a hysteresis
(bang-bang) or a PID control law. Each running thermostat is a climate entity: Home Assistant
and the web server show it and set its mode and target like any other. The file format and the
C++ API are in [components/climate_hub/README.md](../components/climate_hub/README.md).

## What a thermostat does

- **Hysteresis**: heats below the target minus a band and stops above the target plus a band,
  or cools the other way round. **PID**: drives the relay as a slow PWM, from 0 to 100 % of a
  period of minutes.
- **Modes**: off, heat, cool, or heat and cool, as far as its relays allow.
- **Safety**: with no reading yet, a sensor silent for longer than its timeout, or a reading
  above its cut-out temperature, it opens its relays until that clears. A thermostat that
  starts on a sensor that has fallen silent shows its last value but does not act on it.
- **Relay wear**: a minimum on and off time, 10 s each unless set otherwise, counted from the
  relay's last move, whichever thermostat made it. Saving a thermostat does not restart its
  cycle.
- **A relay belongs to the running thermostat.** Switched from anywhere else — the panel,
  Modbus, an automation, Home Assistant — it is put back within a moment. Two thermostats may
  name one relay and take turns, a summer and a winter profile on one boiler; only one of them
  runs at a time, and taking the relay over from the other leaves it as it is.

## Storage

Thermostats are `/littlefs/climates/<id>.json` on the LittleFS partition from
`features/storage.yaml`, one file per thermostat, `id` made from the name when it was created.
The device keeps at most eight. OTA updates keep the files; a factory reset formats the
partition, so the thermostats go with it. A file dropped into the folder, over the file API
under `/files` for instance, is loaded at the next boot.

A thermostat whose sensor or relay is missing at boot stays on disk, not running, until it is
back.

## Home Assistant

- Home Assistant knows an entity by its name, so **renaming a thermostat makes a new entity
  there** and leaves the old one unavailable; its history and automations stay with the old one.
- Starting, stopping, removing or renaming a thermostat, or changing its relays or temperature
  range, makes Home Assistant reconnect so that it sees the change: the whole device shows as
  unavailable for about five seconds, and a log stream over the API drops and reconnects. A
  new target or mode never does this.
- Names are unique on the device: two thermostats cannot share one, nor two names that give the
  same entity id (`Room 1` and `Room_1`), nor a thermostat and a climate from the YAML.
- The room temperature is shown to a tenth of a degree; the target moves in the thermostat's
  own step.

## Testing without hardware

```bash
python tests/run.py climate_hub [-- --gtest_filter='ControlLoop.*']
```

Builds the component for the host platform into a Google Test binary: the control laws, the
relay timing, the file format, and the whole component over a directory that stands in for the
flash, with a clock the test moves. In the emulator, `packages/qemu/climate-plant.yaml` adds a
`QEMU Room Temperature` sensor that `Relay 1` warms, so a thermostat has a room to control
([QEMU](QEMU.md)).
