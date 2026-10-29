# Schedules

Weekly timetables the device runs without a recompile. A schedule names **modes**, each a list
of actions on relays and [thermostats](CLIMATE.md) with waits between them, and a week of switch
points, each a day, a time and a mode. At a point the mode's list runs, and the mode stays in
force until the next point; it carries over midnight, and Sunday's last point runs into Monday.
"Day from 06:30: Living Room to comfort, the boiler pump on, two minutes on the valve open;
Night from 23:00: eco, the valve shut, the pump off" is one schedule. Schedules are created,
changed, switched on and off and removed while the device runs, over HTTP under
`/schedule-editor/api` or from a lambda; a file written into their folder by hand is picked up
at the next boot. The file format and the C++ API are in
[components/schedules/README.md](../components/schedules/README.md).

## Modes and actions

A mode has a name, a colour the dashboard shows it in, and up to 12 actions, run in order:

- A relay goes on or off.
- A thermostat goes on, in the mode it had before it went off; goes off; takes one of its
  presets; or takes a target temperature, which it holds in its range.
- A wait holds the rest of the list for 1 second to 24 hours.

A preset or a target turns a thermostat that is off back on, so a mode made of one preset
starts a thermostat that was off. A preset with a mode of its own sets that mode. A stopped
thermostat takes the values into its settings and stays stopped; started, it runs in them. The
schedule's status notes that the thermostat is stopped. A scheduled change ends a calibration
the thermostat runs, as a change from Home Assistant does
([Thermostats](CLIMATE.md#calibrating-a-pid-thermostat)). A mode with no actions does nothing.

Times are the device's local time, in the timezone it has from Home Assistant or the firmware
([Timezone](../README.md#timezone)).

## When a list runs

- At every switch point, from its first action, even when the relays and thermostats are that
  way already.
- The list of the mode in force runs again, waits included, once the device has a valid time
  after a boot, when the schedule is saved while switched on (a rename too), when it is switched
  on, and when another schedule lets go of a relay or a thermostat it shares (below). A device
  that was off at a switch point catches up when it is back.
- When the clock is changed by more than a minute: set by hand, by Home Assistant or by NTP,
  moved to another timezone, or across a DST switch. The list of the mode in force runs; the
  points in between are not played one by one. When clocks go forward, the mode in force after
  the gap runs; when they go back, the points in the repeated hour run a second time. A smaller
  correction never runs a point twice, and one forward across a point runs it.

A new point stops a list still in a wait, and the new list starts at once. Switching a schedule
off, deleting it or saving it onto other relays and thermostats stops its list and leaves them
as they are.

Until the device has a valid time, nothing runs, and each schedule that is switched on says it
waits for the clock.

A change by hand — from Home Assistant, the display, the dashboard, a rule or an input binding —
is not refused and holds until the schedule acts on that relay or thermostat again: at a switch
point, at a later action of a list still running, when the list runs again as above, or when
the thermostat comes back.

## A relay a running thermostat drives

Such a relay is the thermostat's ([Thermostats](CLIMATE.md#a-running-thermostats-relays)). An
action on it is skipped, the schedule's status names the thermostat, and the log names both.
Once the thermostat frees the relay, the relay takes the last action on it the list of the mode
in force has passed. When the list has not reached one yet, nothing happens until it does.

## A missing relay, thermostat or preset

An action names its relay by object id, its thermostat by id, which a rename leaves as it is,
and a preset by its key. A schedule with an action whose relay, thermostat or preset is not on
the device is saved all the same, with a warning that names the first such action
(`skips mode "Day", action 2: relay "relay_7" is not on this device`), and keeps its file. The
action is skipped and the rest of the list runs; the schedule's status lists every such action.
When a thermostat or a preset comes back, created or restored from a backup, the actions on it
the list has passed apply again, in order, without the waits; one whose preset is still missing
is skipped, and once that preset is back they all apply again. A relay comes back at the next
boot that finds it. On a firmware without thermostats, an action on one says
`This device has no thermostats`.

## One schedule per relay or thermostat

The relays and thermostats a schedule drives are every one any of its modes names, whether a
point picks that mode or not. One enabled schedule drives each. A second one that shares any of
them can be saved switched off; saving it switched on, or switching it on, is refused with
`"Winter" already drives Living Room; switch it off first or take over`, naming the first it
shares. Switching it on with a take-over switches off instead every other one that shares any,
in their files too. A relay or a thermostat that is not on the device still keeps the others
off it.

Two enabled schedules that share one come only from files written into the folder by hand.
Then they are taken by id: each runs unless one before it that runs already drives a relay or a
thermostat it names. One that waits, waits whole and drives none of its relays and thermostats,
so a later schedule may drive them. It starts once the one in its way is switched off, deleted
or saved onto others, and the answer to that change names it; a later one that shares a relay
or a thermostat with it then waits in turn.

## Status

Each schedule says where it stands, the first of these that holds, with a sentence saying why
when it does not run:

| Status | When |
| --- | --- |
| `disabled` | switched off |
| `waiting_for_clock` | the device has no valid time yet |
| `conflict` | a schedule before it by id drives a relay or a thermostat it names |
| `running` | it runs |

Beside the status, its issues list each action the device would skip now: a relay, a thermostat
or a preset not on the device, a firmware without thermostats, or a relay a running thermostat
holds. The editor's list also shows each schedule's relays and thermostats, its mode in force
and since when, and its next switch point, for one switched off too.

## Limits

- 16 schedules on a device, the default of `max_schedules` in the YAML (1 to 32). Creating one
  more is refused until one is deleted.
- 8 modes a schedule, 12 actions a mode, a wait of 1 second to 24 hours.
- 12 switch points a day, and at least one in the week.
- A schedule's name: 1 to 48 characters, in any language, unique on the device whatever the
  case and spacing. The id is made from the name when the schedule is created; a rename keeps
  it.
- A mode's name: 1 to 24 characters, in any language, unique in its schedule whatever the case
  and spacing.
- A schedule's file: 16 KiB.
- The device's free memory. A Save or an import that would leave too little is refused with
  `Not enough free memory on the device to save this schedule`, and nothing changes; an edit
  that makes a schedule no larger always goes through. A boot stops loading schedules before
  memory runs short and leaves the rest of the files as they are; the log names each one, and
  the editor's list says how many there are. Until they are deleted, or a restart with room
  loads them, no new schedule can be added, so a reboot never runs a new one in place of one
  the list did not show. The schedules live in the board's PSRAM, so sixteen of the largest
  kind fit beside eight thermostats; a board whose PSRAM does not come up holds about six of
  them, while ordinary ones still fit sixteen.

The editor's `schema` route reports the limits of the firmware at hand and the colours a mode
may have.

## Storage

Schedules are `/littlefs/schedules/<id>.json` on the LittleFS partition from
`features/storage.yaml`, one file per schedule. Nothing is written at a switch point. OTA
updates keep the files; a factory reset formats the partition, so the schedules go with it.
Backup and Restore on the dashboard carry them. A file dropped into the folder, over the file
API under `/files` for instance, is loaded at the next boot. One that breaks the format's rules
is not loaded and is left as it is, and past the limit, or the memory, the files load in name
order.

A file written by a newer firmware, after a rollback say, loads and runs, but this firmware
never writes it: a Save is refused with a sentence that says a newer firmware wrote it,
switching the schedule on or off lasts until the next reboot, and deleting it still works.

## Over HTTP

`features/schedule-editor.yaml` serves the schedules on the web server port under
`/schedule-editor/api`: list them with their status and issues, their relays and thermostats,
the mode in force, the next switch point and the device's clock; read, create, change, switch
on or off and delete one; bring one back from a backup under its own id; and list the relays and
thermostats an action can drive, with the thermostats' presets. The routes and their contract
are in [components/web_schedule_editor/openapi.yaml](../components/web_schedule_editor/openapi.yaml),
the usage in [its README](../components/web_schedule_editor/README.md).

Every change of a schedule moves its revision on: a Save, switching it on or off, a take-over
that switches it off, bringing it back from a backup. An editor page opened before such a change
can no longer save over it: its Save is refused with `The device changed this schedule since it
was read; reload it` until it reloads, so it never undoes what another page, a phone or a
restore did. A Save without a revision, from `curl` say, is not checked.

## Testing without hardware

```bash
python tests/run.py schedules [-- --gtest_filter='Lists.*']
python tests/run.py schedules_no_climate
python tests/run.py web_schedule_editor
```

The first builds the engine for the host platform into a Google Test binary: the file format
and the week, the lists and their waits against a clock the test sets, moves to another zone
and across DST, relays and thermostats as targets, and the whole component over a directory
that stands in for the flash. The second builds it on a firmware without thermostats. The third
drives every HTTP route through the handler, over the real engine and a stand-in for the web
server, and runs the cases the dashboard's client mock shares
([TESTING.md](TESTING.md#schedule-editor-cases)).
