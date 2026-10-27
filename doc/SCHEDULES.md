# Schedules

Weekly timetables the device runs without a recompile. A schedule drives one target, a relay or
a [thermostat](CLIMATE.md), through the week. It is a list of switch points, each a day, a time
and a value: from a point on, the target has that point's value until the next point. The value
in force carries over midnight, and Sunday's last point runs into Monday. "Comfort from 07:00,
eco from 22:00 on weekdays" is one schedule. Schedules are created, changed, switched on and off
and removed while the device runs, over HTTP under `/schedule-editor/api` or from a lambda; a
file written into their folder by hand is picked up at the next boot. The file format and the
C++ API are in [components/schedules/README.md](../components/schedules/README.md).

## Values

- A relay takes on or off.
- A thermostat takes one of its presets, off, or a target temperature, which it holds in its
  range.
- A preset or a target turns a thermostat that is off back on, in the mode it was in before it
  went off, so a day can go from off to comfort. A preset with a mode of its own sets that mode.
- A stopped thermostat takes the values into its settings and stays stopped; started, it runs
  in them. The schedule's status notes that the thermostat is stopped.
- A scheduled change ends a calibration the thermostat runs, as a change from Home Assistant
  does ([Thermostats](CLIMATE.md#calibrating-a-pid-thermostat)).

Times are the device's local time, in the timezone it has from Home Assistant or the firmware
([Timezone](../README.md#timezone)).

## When a value is applied

- At every switch point, even when the target has that value already.
- When the schedule starts: once the device has a valid time after a boot, when it is saved
  while switched on (a rename too), when it is switched on, when another schedule lets go of its
  target (below), and when its relay, thermostat or preset is back. The value in force applies
  at once, so a device that was off at a switch point catches up when it is back.
- When the clock is changed by more than a minute: set by hand, by Home Assistant or by NTP,
  moved to another timezone, or across a DST switch. The value in force applies; the points in
  between are not played one by one. When clocks go forward, the value in force after the gap
  applies. When they go back, the points in the repeated hour apply a second time. A smaller
  correction never applies a point twice, and one forward across a point applies it.

Until the device has a valid time, nothing is applied, and each schedule that is switched on
says it waits for the clock.

A change by hand — from Home Assistant, the display, the dashboard, a rule or an input binding —
is not refused and holds until the next switch point. A reboot, a Save of the schedule while it
is switched on, or switching it off and on again puts the scheduled value back. Switching a
schedule off, deleting it or saving it onto another target leaves the target as it is.

## A relay a running thermostat drives

Such a relay is the thermostat's ([Thermostats](CLIMATE.md#a-running-thermostats-relays)). A
schedule leaves it alone, its status names the thermostat, and the log names both. Once the
thermostat frees the relay, the relay takes the value in force at once.

## A missing relay, thermostat or preset

A schedule names its relay by object id, its thermostat by id, which a rename leaves as it is,
and a preset by its key. A schedule whose relay, thermostat or one of whose presets is not on
the device is saved all the same, with a warning, and keeps its file. It does not run, its
status says what is missing (`Thermostat "attic" is not on this device`), and it runs again,
with the value in force, once that is back: a thermostat or a preset as soon as it is created or
restored from a backup, a relay at the next boot that finds it. On a firmware without
thermostats, a schedule for one says `This device has no thermostats`.

## One schedule per target

One enabled schedule drives a target. A second one for the same relay or thermostat can be
saved switched off; saving it switched on, or switching it on, is refused with
`"Winter" already drives Living Room; switch it off first or take over`. Switching it on with a
take-over switches the other one off instead, in its file too. A schedule whose target is missing
still keeps the others off that target.

Two enabled schedules on one target come only from files written into the folder by hand. Then
the first by id drives the target and the other waits; it starts as soon as the first is
switched off, deleted or saved onto another target, and the answer to that change names it.

## Status

Each schedule says where it stands, the first of these that holds, with a sentence saying why
when it does not run:

| Status | When |
| --- | --- |
| `disabled` | switched off |
| `waiting_for_clock` | the device has no valid time yet |
| `missing_target` | its relay or thermostat is not on the device, or the firmware has no thermostats |
| `missing_preset` | its thermostat has no preset a point names |
| `conflict` | another enabled schedule drives its target |
| `held` | a running thermostat drives its relay |
| `running` | it drives its target |

The editor's list also shows each schedule's value in force and its next switch point, for one
switched off too.

## Limits

- 16 schedules on a device, the default of `max_schedules` in the YAML (1 to 32). Creating one
  more is refused until one is deleted.
- 12 switch points a day, and at least one in the week.
- A name of 1 to 48 characters, in any language, unique on the device whatever the case and
  spacing. The id is made from the name when the schedule is created; a rename keeps it.

The editor's `schema` route reports the limits of the firmware at hand.

## Storage

Schedules are `/littlefs/schedules/<id>.json` on the LittleFS partition from
`features/storage.yaml`, one file per schedule. Nothing is written at a switch point. OTA
updates keep the files; a factory reset formats the partition, so the schedules go with it.
Backup and Restore on the dashboard carry them. A file dropped into the folder, over the file
API under `/files` for instance, is loaded at the next boot. One that breaks the format's rules
is not loaded and is left as it is, and past the limit the files load in name order.

A file written by a newer firmware, after a rollback say, loads and runs, but this firmware
never writes it: a Save is refused with a sentence that says a newer firmware wrote it,
switching the schedule on or off lasts until the next reboot, and deleting it still works.

## Over HTTP

`features/schedule-editor.yaml` serves the schedules on the web server port under
`/schedule-editor/api`: list them with their status, the value in force, the next switch point
and the device's clock; read, create, change, switch on or off and delete one; bring one back
from a backup under its own id; and list the relays and thermostats a schedule can drive, with
the thermostats' presets. The routes and their contract are in
[components/web_schedule_editor/openapi.yaml](../components/web_schedule_editor/openapi.yaml),
the usage in [its README](../components/web_schedule_editor/README.md).

Every change of a schedule moves its revision on: a Save, switching it on or off, a take-over
that switches it off, bringing it back from a backup. An editor page opened before such a change
can no longer save over it: its Save is refused with `The device changed this schedule since it
was read; reload it` until it reloads, so it never undoes what another page, a phone or a
restore did. A Save without a revision, from `curl` say, is not checked.

## Testing without hardware

```bash
python tests/run.py schedules [-- --gtest_filter='Tick.*']
python tests/run.py schedules_no_climate
python tests/run.py web_schedule_editor
```

The first builds the engine for the host platform into a Google Test binary: the file format
and the week, when values apply against a clock the test sets, moves to another zone and across
DST, relays and thermostats as targets, and the whole component over a directory that stands in
for the flash. The second builds it on a firmware without thermostats. The third drives every
HTTP route through the handler, over the real engine and a stand-in for the web server, and runs
the cases the dashboard's client mock shares
([TESTING.md](TESTING.md#schedule-editor-cases)).
