# Runtime Automations

Rules that run on the device without recompiling. They live as JSON files on the LittleFS
partition, are loaded at boot and can be created, changed and removed while the device runs:
over HTTP under `/automation-editor/api` ([the routes](../components/web_automation_editor/README.md)),
from a lambda, or by writing into the folder by hand. The file format and the C++ API are in
[components/automations/README.md](../components/automations/README.md).

## What a rule can do

- **Triggers** (any of them fires the rule): input press / release / click / state change,
  switch turn on / off / state change, a sensor above / below / in range (fires on the
  crossing, once), cron (six fields, seconds first), startup, and condition change.
- **Condition change** makes the rule a state rather than an event: "relay 1 is on while
  (input 1 and input 2) or not input 3" is one rule. It fires whenever the condition's result
  changes, running the actions on true and the `else` actions on false, and once with the
  current result when the rule starts watching: at boot, when it is added, changed or enabled,
  and when it is built again because an input or a thermostat it names came back. An input
  with no state yet reads as off, and a sensor with no reading as outside every range. A rule
  with this trigger needs a condition, and wants mode `restart`: in `single`, a change that comes
  while the rule still runs, in a delay or with a thermostat action waiting for the next loop
  pass, is ignored.
- **Condition** (optional): input is on/off, switch is on/off, a sensor above / below / in
  range (`above` and `below` are strict, a range includes both ends), and `and` / `or` / `xor`
  groups of those, nested. When false, the `else` actions run. A switch condition reads any
  switch a rule may name, the one the rule drives included: with "only while relay 1 is off",
  the rule turns relay 2 on only while relay 1 is off. Without a condition change trigger it is
  read once, when the rule starts, and Run now skips it; with one the rule follows it. Either way
  Home Assistant, the panel, Modbus and other rules can still turn relay 2 on.
- **Actions**: switch turn on / off / toggle / follow (copies the state the trigger
  carried, optionally inverted), delay. A switch action on a relay a running thermostat drives
  does nothing; the log names the rule and the thermostat, and the rest of the rule runs
  ([Thermostats](CLIMATE.md#a-running-thermostats-relays)). When the thermostat lets the relay
  go, a rule with a condition trigger sets it where its branch for the current result leaves
  it (its steps on that relay played through: a toggle turns over the step before it, and one
  with no step before it sets nothing), so the relay goes where the condition says; nothing
  else in the rule plays again. While a run is under way, the
  relay goes where the run has got to, and the run sets it further when it reaches its next step
  on it, after its delays.
- **Thermostat actions**: turn off, turn on (back in the mode it was in before it went off),
  pick a preset, set the target, and follow: one preset or on/off while the trigger's state is
  on, another while it is off. A preset that keeps the mode does not turn an off thermostat on:
  for on and off plus a preset, use two actions. They act on the thermostat whether it runs or
  not, on the next loop pass. A new target or mode ends a calibration the thermostat runs, as
  one from Home Assistant does ([Thermostats](CLIMATE.md#calibrating-a-pid-thermostat)).
- **Mode**: `single` ignores a trigger while the rule is running, `restart` starts over,
  `parallel` runs up to 8 copies. A trigger `single` ignores still hands its state to a follow
  that has not played yet, so the target ends where the trigger is.
- **Run now**: plays a rule's actions at once, skipping its triggers and its condition, even
  while the rule is disabled; the mode applies as for a trigger, so `single` refuses while a
  run is under way. A follow has no trigger state to copy unless a trigger arrives while the
  run plays. A rule with a condition trigger plays the branch its condition gives now instead,
  as the trigger would. A rule that is not built cannot run.

Rules that keep setting each other off, such as one toggling the switch that triggers it, are
cut 8 nested events deep: the next event is dropped, with a warning naming the rule, and the
switches stay where the loop left them until something sets it off again, which is cut the same
way.

A sensor is any numeric one that is not internal, not only a temperature, and its thresholds
are in its own unit. A switch is any that is not internal and not one of the device's own
settings, such as **Allow uploads from ESPHome**: a rule naming one of those is not built.

Cron follows the device's clock, wherever its time comes from. A jump forward of up to 15
minutes fires every cron time it skipped at once, and a longer one skips them; a jump back of
more than 15 minutes fires the times in between again as the clock passes them, and a shorter
one fires nothing until the clock is back where it was.

A DST switch is not a jump. A cron time whose hour field is `*` (every hour, every few minutes)
keeps to the time that passes. One at given hours (daily, weekly, monthly) runs once for the
hour the clocks skip, right after they go forward, and does not run again in the hour they
repeat when they go back, a restart inside that hour included; a device that was off when the
clocks went back runs that hour's times again. A new time zone more than 15 minutes and up to an
hour away counts as a DST switch, one further away as a jump.

A timetable, what relays and thermostats do by the time of day and the day of the week, is a
[schedule](SCHEDULES.md) rather than a rule.

## Storage

Rules are `/littlefs/automations/<name>.json` on the LittleFS partition from
`features/storage.yaml`, one file per rule, named after the rule. Two rules cannot share a
name, whatever the spacing and the case of Latin, Greek and Cyrillic letters: `Свет на кухне`
and `СВЕТ НА КУХНЕ` are one. OTA updates keep the files; a factory reset —
**Settings → Factory reset → Confirm** on
the display, the FN button held for 10 seconds, or `POST /api/device/system/factory-reset` —
formats the partition, so the rules go with it.

Entities are named by object id, so renaming a relay, an input or a sensor leaves
the rules that used it unbuilt — the boot log says which, and their files are kept exactly as
written until the entity is back. A [virtual input](ENTITY_SETTINGS.md#virtual-inputs) comes and
goes while the device runs, and the rules that name it follow: a delete stops them, files kept,
and a virtual input set up in that slot starts them again.

A thermostat is named by its id, which a rename leaves as it is, and a preset by its key. A
rule naming a thermostat or a preset that is not there cannot be saved. One that loses it —
the thermostat deleted, the preset removed — stops running and keeps its file, and runs again
as soon as a thermostat or a preset with that id or key is back, created or restored from a
backup. The editor's rule list says which rules do not run and why.

## Over HTTP

`features/automation-editor.yaml` serves the rules on the web server port under
`/automation-editor/api`: the routes and their contract are in
[components/web_automation_editor/openapi.yaml](../components/web_automation_editor/openapi.yaml),
the usage in [its README](../components/web_automation_editor/README.md). The rule list says
which rules are running and how long ago each last ran, from a trigger or Run now; a run with no
action to play does not count, and none of it is kept across a reboot.

## Testing without hardware

```bash
python tests/run.py automations [-- --gtest_filter='Storage.*']
python tests/run.py web_automation_editor
python tests/run.py automations_no_climate
```

Builds the engine for the host platform into a Google Test binary and runs
`tests/components/automations/cases/`:
the JSON and cron parsers, rules driven by hand with every delay held back until the test fires
it, the cron tick against a clock the test moves and switches for DST, and the whole component over a directory that
stands in for the flash. The second suite drives every HTTP route through the handler, over the
real engine and a stand-in for the web server. The third builds both on a firmware without
thermostats. How to add a case: [TESTING.md](TESTING.md).
