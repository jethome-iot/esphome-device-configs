# Thermostats

Thermostats the device runs without a recompile. Each one reads a temperature sensor in °C — a
`Temp N` probe, say — and switches a heating relay, a cooling relay or both, with a hysteresis
(bang-bang) or a PID control law. Each running thermostat is a climate entity: Home Assistant
and the web server show it and set its mode and target like any other. Thermostats are
created, changed, started, stopped and removed while the device runs, over HTTP under
`/climate-editor/api` or from a lambda; a file written into their folder by hand is picked up
at the next boot. The file format and the C++ API are in
[components/climate_hub/README.md](../components/climate_hub/README.md).

## What a thermostat does

- **Hysteresis**: heats below the target minus a band and stops above the target plus a band,
  or cools the other way round. **PID**: drives the relay as a slow PWM, from 0 to 100 % of a
  period of minutes.
- **Modes**: off, heat, cool, or heat and cool, as far as its relays allow.
- **Safety**: it keeps its relays open until its sensor's first reading, and waits for one
  as long as the sensor timeout before it reports a fault. A sensor silent for longer than its
  timeout, or a reading above the cut-out temperature, opens the relays until that clears. A
  thermostat that starts on a sensor that has fallen silent shows its last value but does not
  act on it. A reading that is not a number, or an infinite one, counts as no reading at all.
- **Relay wear**: a minimum on and off time, 10 s each unless set otherwise, counted from the
  relay's last move, whichever thermostat made it. Saving a thermostat does not restart its
  cycle, and a PID keeps what it has learnt unless the Save changes its control law or its
  sensor.
- **A relay belongs to the running thermostat**, in mode off too, which keeps it open; only
  stopping the thermostat frees it. What the rest of the device does with such a relay is
  below. Two thermostats may name one relay and take turns, a summer and a winter profile on
  one boiler; only one of them runs at a time, and taking the relay over from the other leaves
  it as it is. One that could not run, its sensor or a relay missing, cannot take it over.

## A running thermostat's relays

What everything else that switches a relay does with one a running thermostat drives:

| Writer | On a relay a running thermostat drives |
| --- | --- |
| The panel: CENTER on the status page, **Relays → Relay N → State** | leave it alone; the log names the thermostat |
| Modbus coils `0x0000`–`0x0005` | a write that would move it answers exception `0x04`; writing the state it already has is accepted |
| Automation rules, `switch` actions | leave it alone and log the rule and the thermostat; the rest of the rule runs |
| Input bindings, Toggle and Follow | leave it alone; once the thermostat frees it, a Follow relay takes its input's state at once |
| Home Assistant, the web server's REST | switch it, and the thermostat puts it back within a moment; the dashboard locks its own toggle |
| **Inverted** in the relay's settings, on the panel or the dashboard | refused; the dashboard's answer and the panel's log name the thermostat. Start mode and the binding change as usual |

**The boot pulse.** A relay whose Start mode is On, or Last when it was on, closes when the
device starts, before any thermostat runs, and stays closed until the thermostat that drives it
takes it over at its first pass, a moment later; an input binding does not move it in between.
Start mode Off on that relay avoids the pulse.

## Storage

Thermostats are `/littlefs/climates/<id>.json` on the LittleFS partition from
`features/storage.yaml`, one file per thermostat, `id` made from the name when it was created.
The device keeps at most eight. OTA updates keep the files; a factory reset formats the
partition, so the thermostats go with it. A file dropped into the folder, over the file API
under `/files` for instance, is loaded at the next boot.

A thermostat whose sensor or relay is missing, at boot or when it is saved or switched on, stays
enabled on disk but does not run; the Save or the switch-on succeeds with a warning that names
what is missing. It starts at the next boot that finds what it names, or at a Save or a
switch-on that finds it there. Until then, or until it is switched off, the device's log says
why it waits: when the start fails, and again in the configuration it prints whenever a log
viewer connects. Over HTTP, the list and the status say it in their `waiting` field, in the
words of the warning.

## Over HTTP

`features/climate-editor.yaml` serves the thermostats on the web server port under
`/climate-editor/api`: list them, read, create, change and delete one, start or stop it, move
its target, and watch what each one is doing. The routes and their contract are in
[components/web_climate_editor/openapi.yaml](../components/web_climate_editor/openapi.yaml),
the usage in [its README](../components/web_climate_editor/README.md).

A Save or a start over `enable` of a thermostat that is to run is refused while another running
thermostat drives its relay, or when its sensor does not report °C; one whose sensor or relay is
missing is stored enabled and waits, as above. A start can take the relay over instead of being
refused, which stops the other thermostat and stores it as disabled; one whose sensor or relay
is missing cannot.

## On the display

**Thermostats** in the display menu of `jxd-r6-e1eth-lcd` lists the thermostats the device
booted with, each with what its sensor reads; one created later shows after a reboot. A row
opens its name, the reading, what it is doing (Heating, Cooling, Idle, Off, Waiting, Disabled,
or in a word the fault that keeps it from controlling), the target and Enabled. The target moves
in the thermostat's own step inside its range; Enabled starts or stops it like the start over
HTTP, never taking a relay over: an On that is refused stays Off, and the log says why.

## Home Assistant

- Home Assistant knows an entity by its name, so **renaming a thermostat makes a new entity
  there** and leaves the old one unavailable; its history and automations stay with the old one.
- Home Assistant reconnects to see the change when a thermostat starts or stops (removing a
  running one stops it), and when a running one is renamed, gains or loses its heating or
  cooling relay, or gets a new temperature range or step: the whole device shows as unavailable
  for about five seconds, and a log stream over the API drops and reconnects. Swapping one
  relay for another, a new target, mode, band or gains, and any change to a thermostat that is
  not running never do this.
- Names are unique on the device: two thermostats cannot share one, nor two names that give the
  same entity id (`Room 1` and `Room_1`), nor a thermostat and a climate from the YAML.
- The room temperature is shown to a tenth of a degree; the target moves in the thermostat's
  own step.
- A running thermostat that neither heats nor cools shows as idle. Off means mode off, a fault,
  or a thermostat that is not running.

## Testing without hardware

```bash
python tests/run.py climate_hub [-- --gtest_filter='ControlLoop.*']
python tests/run.py web_climate_editor
```

Builds the component for the host platform into a Google Test binary: the control laws, the
relay timing, the file format, and the whole component over a directory that stands in for the
flash, with a clock the test moves. The second suite drives every HTTP route through the
handler, over the real component and a stand-in for the web server. In the emulator,
`packages/qemu/climate-plant.yaml` adds a `QEMU Room Temperature` sensor that `Relay 1` warms,
so a thermostat has a room to control ([QEMU](QEMU.md)).
