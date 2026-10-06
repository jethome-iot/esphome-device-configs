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
- **Presets**: up to eight per thermostat, each a name, a target and a mode or "keep the
  current one". Picking one, from Home Assistant, the web server or the editor's API, sets its
  target and its mode; a stopped thermostat keeps the pick and starts in it. A target or a mode
  set by hand afterwards keeps it shown as the active preset.
  Editing the active preset's values applies them at once, and the active preset comes back
  after a reboot.
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
  one boiler; only one of them is switched on at a time, and taking the relay over from the
  other switches the other off and leaves the relay as it is. One that could not run, its
  sensor or a relay missing, cannot take it over.

## Calibrating a PID thermostat

A PID thermostat can find its own gains. A calibration, started from the editor or over HTTP,
swings the room around its target: the relay closes fully once the room is a quarter of a degree under the
target and opens a quarter of a degree over it, so the room goes about half a degree to a degree
either side, more where the heat is slow to arrive. The relay's minimum on and off times and the
cut-out temperature hold throughout. A room with radiators takes about an hour and a half, a
floor heating eight to ten hours. In heat and cool mode the start asks which relay to swing; the
other one stays open. Home Assistant sees the thermostat heating or idle, as at any other time.

The device then turns what it measured into the gains of the rule picked at the start
(Ziegler-Nichols PI unless another), writes them into the thermostat and runs with them. An
editor page opened before can no longer save over them: its Save is refused until it reloads.
The result, the gains it replaced and any warning about it stay in the thermostat's status until
the next calibration, a delete or a reboot.

A calibration ends without new gains when it is cancelled, when the target or the mode changes
from anywhere (Home Assistant, the web server, the editor, a rule, a preset), when the thermostat
is saved, switched off, deleted or taken over, on any fault, after 24 hours, after 6 hours
without a relay switch (a heater that cannot cross the band), or when a noisy probe crosses the
target more than 64 times. A reboot ends it too. The status says which, and the thermostat goes
back to its PID with the gains it had.

## A running thermostat's relays

What everything else that switches a relay does with one a running thermostat drives:

| Writer | On a relay a running thermostat drives |
| --- | --- |
| The panel: CENTER on the status page, **Relays → Relay N → State** | leave it alone; the log names the thermostat |
| Modbus coils `0x0000`–`0x0005` | a write that would move it answers exception `0x04`; writing the state it already has is accepted |
| Automation rules, `switch` actions | leave it alone and log the rule and the thermostat; the rest of the rule runs |
| Input bindings, Toggle and Follow | leave it alone; once the thermostat frees it, a Follow relay takes its input's state at once, unless a thermostat that waited for the relay starts on it |
| Home Assistant, the web server's REST | switch it, and the thermostat puts it back, as below; the dashboard locks its own toggle |
| **Inverted** in the relay's settings, on the panel or the dashboard | refused; the dashboard's answer and the panel's log name the thermostat. Start mode and the binding change as usual |

**Switched from elsewhere.** A relay switched by Home Assistant or the REST API is put back
within a moment. Switched again, it stays there for its minimum on or off time, 10 s at least,
before it is put back, so whatever keeps switching it cannot make it chatter. Once five
switches come without ten quiet minutes after a put-back between them, the thermostat reports
`relay_contested`: it goes on working, and the fault clears ten minutes after the last
put-back. Until the sensor's first reading, while it is silent or while the room is above the
cut-out temperature, a relay switched on from elsewhere is switched off again at once, every
time.

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

A file written by a newer firmware, after a rollback say, loads and runs as far as this one
understands it, but this firmware never writes it: a change from Home Assistant, the panel or a
rule lasts until the next reboot, and a Save from the editor is refused with a sentence that
says a newer firmware wrote it. Starting it in another thermostat's place lasts until the next
reboot too: the ones it took the relay from, running or waiting, stay enabled in their files.
Deleting the thermostat still works. A newer file that breaks this firmware's rules — a preset
mode it does not know, more than eight presets, a control law it does not have — is not loaded
and is left as it is, as any file that breaks them. To a firmware from before calibration, every
thermostat file this one writes is such a newer file: rolled back, a calibrated thermostat runs
its gains as far as that firmware's ranges go, and its file keeps them for the next update.

A thermostat whose sensor or relay is missing, at boot or when it is saved or switched on, stays
enabled on disk but does not run; the Save or the switch-on succeeds with a warning that names
what is missing. It starts at the next boot that finds what it names, or at a Save or a
switch-on that finds it there. Until then, or until it is switched off, the device's log says
why it waits: when the start fails, and again in the configuration it prints whenever a log
viewer connects. Over HTTP, the list and the status say it in their `waiting` field, in the
words of the warning.

Two thermostats switched on for one relay come only from files written by hand or a restore.
The boot runs the first by id, and the other waits for the relay: it starts as soon as the
first is switched off, removed or saved onto another relay, and the answer to that change names
it.

## Over HTTP

`features/climate-editor.yaml` serves the thermostats on the web server port under
`/climate-editor/api`: list them, read, create, change and delete one, edit and pick its
presets, start or stop it, move its target, calibrate it, and watch what each one is doing and
which preset is active. The routes and their contract are in
[components/web_climate_editor/openapi.yaml](../components/web_climate_editor/openapi.yaml),
the usage in [its README](../components/web_climate_editor/README.md).

A Save or a start over `enable` of a thermostat that is to run is refused while another
thermostat that is switched on names its relay, running or waiting, or when its sensor does not
report °C; one whose sensor or relay is missing is stored enabled and waits, as above. A start
can take the relay over instead of being refused, which stores the other thermostats on it as
disabled and stops the one that runs; one whose sensor or relay is missing cannot.

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
  cooling relay, gets a new temperature range or step, gains or loses one of Home Assistant's
  own presets, or has one of its other presets added, removed, renamed or moved: the whole
  device shows as unavailable for about five seconds, and a log stream over the API drops and
  reconnects. Swapping one relay for another, a new target, mode, band or gains, new values for
  a preset, one of Home Assistant's own presets written in another case (`Eco` as `ECO`) or
  moved in the list, and any change to a thermostat that is not running never do this.
- A preset named like one of Home Assistant's own (eco, away, boost, comfort, home, sleep,
  activity, in any case) is that preset there, translated in its interface; any other name is
  shown as it is. `none` is Home Assistant's word for no preset and cannot be a name.
- Names are unique on the device: two thermostats cannot share one, nor two names that give the
  same entity id (`Room 1` and `Room_1`), nor a thermostat and a climate from the YAML.
- The room temperature is shown to a tenth of a degree; the target moves in the thermostat's
  own step.
- A running thermostat that neither heats nor cools shows as idle. Off means mode off, a fault
  other than `relay_contested`, or a thermostat that is not running.

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
