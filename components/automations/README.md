# automations

Rules the device runs without a recompile. They are JSON files on a filesystem storage, loaded
at boot into entity pointers and driven by entity state callbacks, a one-second cron tick and a
startup event. Rules can be added, changed, enabled and removed while the device runs.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, case_fold, automations, loop_job, switch_hold]

i2c:
  sda: 5
  scl: 4

time:
  - platform: pcf8563
    id: pcf8563_time

littlefs_storage:
  id: user_storage

automations:
  storage: user_storage
  time_id: pcf8563_time
```

With `web_server` on ESP32, give the loop task 12288 B, `esp32: framework: advanced:
loop_task_stack_size: 12288`: rules that set each other off nest 8 deep on that stack, and while
a client holds `/events` open each relay state they change is sent from there too, which
overflows ESPHome's default 8192 B (#345). Below 12288 B the build warns.

## Options

| Option        | Default       | Meaning                                                                    |
| ------------- | ------------- | -------------------------------------------------------------------------- |
| `storage`     |               | A `filesystem_storage_abstract` backend, `littlefs_storage` on a device; required |
| `time_id`     |               | A `time` platform. Without one cron triggers are refused at boot, the rest still run |
| `folder_path` | `automations` | The folder below the backend's base path that holds the rule files         |

## A rule

```json
{
  "id": 1,
  "name": "Porch light",
  "enabled": true,
  "mode": "single",
  "triggers": [{"source": "input", "type": "click", "object_id": "input_1"}],
  "condition": {"type": "input", "object_id": "input_3", "state": "true"},
  "actions": [
    {"source": "switch", "type": "turn_on", "object_id": "relay_1"},
    {"source": "delay", "delay_ms": 5000},
    {"source": "switch", "type": "turn_off", "object_id": "relay_1"}
  ]
}
```

A name is any text in any script, without control characters. Entities are named by their
object id. Any trigger fires the rule; the condition, when there is one, picks `actions` or
`else_actions`.

| Key                 | Values                                                                      |
| ------------------- | --------------------------------------------------------------------------- |
| `triggers[].source` | `input` (`press`, `release`, `click`, `state_change`), `switch` (`turn_on`, `turn_off`, `state_change`), `temperature` — any numeric sensor, despite the key (`below` / `above` with `threshold`, `range` with `min_threshold` and `max_threshold`, in the sensor's unit), `cron`, `startup`, `condition` |
| `condition.type`    | `input` and `switch` with `state` (`"true"` when absent), `temperature` with `temperature_type` — any numeric sensor, as for the trigger (`below` / `above` with `threshold`, `range` with `min_threshold` and `max_threshold`), and `and` / `or` / `xor` over a `conditions` list, nested freely |
| `actions[].source`  | `switch` (`turn_on`, `turn_off`, `toggle`, `follow` with `invert`), `delay` with `delay_ms`, `climate` with `climate` (`turn_on`, `turn_off`, `set_preset` with `preset`, `set_target` with `target`, `follow` with `on` and `off`) |
| `mode`              | `single` ignores a trigger while the rule runs, `restart` starts over, `parallel` runs up to 8 copies |
| `enabled`           | `true` when absent; a disabled rule is loaded and listed but never fires |
| `cron_preset`       | The editor's own note about the form it offered: `daily`, `hourly`, `every_n_minutes`, `weekly`, `monthly`, `custom`. The engine never reads it, and writes it back only when the file had one |

A `click` is a press between 200 and 1000 ms. A `temperature` trigger fires on the crossing and
arms again when the value goes back. `above` and `below` are strict, a range includes both ends,
for triggers and conditions alike. `follow` drives its target from the state the trigger
carried; in `single` mode a trigger ignored while the rule runs still hands its state to a
`follow` that has not played yet. A `switch` condition reads the switch as it is when the rule
fires, the one the rule drives included, so `"state": "false"` on another relay keeps the rule
from driving its own while that one is on. It is read once, when the run starts, and holds back
neither `run_automation()` nor any other writer of the relay. A `switch` action on a relay a running thermostat drives (a
[`switch_hold`](../switch_hold/switch_hold.h) holder) does nothing but log the rule and the
thermostat, and the run goes on. `cron` is six fields, seconds first — `"*/2 * * * * *"`,
`"0 30 6,18 1 * *"` — with `*`, `*/N`, `X-Y`, `X-Y/N` and lists; a field that matches nothing
is rejected.

A jump of the `time_id` clock back by more than 15 minutes is not handled: the moments it
passes again fire a second time.

A DST switch, the local offset moving by more than 15 minutes and at most an hour while the
time runs on, is not a jump. A `cron` trigger whose hours field is `*` keeps to the time that
passes. A rule's triggers at given hours run it once, at the switch, for the times the clocks
skip forward, and not again for the times they repeat going back. What has run is kept in one
preference record, written when the clocks go back, so a restart inside the repeated hour keeps
it; a device off at the switch has no record and runs the repeated times again.

## Condition triggers

A `condition` trigger takes no parameters and makes the rule a state rather than an event. It
fires whenever the condition's result changes, and once with the current result when the rule
starts watching: at the startup event, when it is added, updated or enabled after it, and when
it is built again because an input or a thermostat it names came back. `actions` run on true and
`else_actions` on false, the result being the state the run carries, so a switch that mirrors
the condition is a `turn_on` in one and a `turn_off` in the other, and a `follow` in each copies
the result. The engine watches every entity the condition reads, an input's first state and its
loss included; an input with no state reads as off, and a sensor with no reading is outside
every range. A rule with this trigger and no `condition` is not built (`Trigger 1: no condition
to watch`). Give it `restart`: in `single`, a change that comes while a delay runs is ignored.

Run now plays the branch the condition gives at that moment. When a running thermostat lets a
relay go, a watching rule sets it where its branch for the current result leaves it, the last
`turn_on`, `turn_off` or `follow` on that relay, as a binding's `follow` does; a `toggle` sets
nothing, and the rest of the branch does not play again.

A condition may read the switch the rule drives. Input 1, start, turns relay 1 on and the relay
holds itself on; input 2, stop, turns it off, input 1 held or not, as a motor's stop button wins
over its start button:

```json
{
  "name": "Latch",
  "mode": "restart",
  "triggers": [{"source": "condition"}],
  "condition": {"type": "and", "conditions": [
    {"type": "or", "conditions": [
      {"type": "input", "object_id": "input_1", "state": "true"},
      {"type": "switch", "object_id": "relay_1", "state": "true"}
    ]},
    {"type": "input", "object_id": "input_2", "state": "false"}
  ]},
  "actions": [{"source": "switch", "type": "turn_on", "object_id": "relay_1"}],
  "else_actions": [{"source": "switch", "type": "turn_off", "object_id": "relay_1"}]
}
```

A rule whose result its own actions keep flipping, `relay_1 = NOT relay_1`, is cut at the
nesting bound like any other loop; the rules on that chain take the result they stop at as seen,
so only a real change sets the loop off again.

## Thermostat actions

A `climate` action acts on a [`climate_hub`](../climate_hub/README.md) thermostat, running or
not, as Home Assistant would. A firmware without `climate_hub` has no thermostat to name, so a
rule with such an action is never built there.

```json
{"source": "climate", "type": "set_preset", "climate": "living-room", "preset": "eco"}
{"source": "climate", "type": "follow", "climate": "living-room",
 "on": {"type": "set_preset", "preset": "comfort"}, "off": {"type": "set_preset", "preset": "eco"}}
```

- `climate` is the thermostat's id, the name of its file, never its entity: renaming the
  thermostat leaves the rule working. `preset` is a preset's key.
- `turn_off` sets mode off. `turn_on` sets the mode it was in before it went off, which its file
  keeps across a reboot.
- `set_target` sets the target to `target`, in °C, held inside the thermostat's range.
- `follow` takes `on` while the state the trigger carried is on and `off` while it is off, each
  `{"type": "turn_on"}`, `{"type": "turn_off"}` or `{"type": "set_preset", "preset": "<key>"}`.
  A trigger that carries no state leaves the thermostat alone. A preset that keeps the mode does
  not turn an off thermostat on: for on and off plus a preset, use two actions.
- A thermostat moves on the next loop pass, not inside the callback that fired the rule; the
  actions after it in the run follow on that pass. A rule restarted or stopped before then does
  not move it.

A switch action calls the rules that trigger on that switch before it returns. Such calls are
cut 8 deep: the event that would go deeper reaches no rule, and a warning names the innermost
rule.

## Storage

One file per rule, `<base_path>/<folder_path>/<name>.json`, the name case-folded (its Latin, Greek
and Cyrillic letters) with runs of spaces, dashes and underscores collapsed into one `_`; past 48
bytes it is cut and ends in `-<id>`. Two rules cannot have names that come out the same before
that cut.

The folder is meant to be writable by hand, so what it holds is repaired at boot: a rule in a
file named after something else is moved to its own file, a missing or repeated `id` is
restamped, and a second rule claiming a taken name becomes `<name> 2`.

A file that is empty, larger than 16 KiB, not valid JSON, or that spells any of the words above
in a way the engine does not know is refused whole and left exactly as it is: the log names the
word and the file. Nothing is loaded from it, so a rule with one typo never runs half of what it
says — and a repair never writes the engine's guess over what its author wrote.

A removal or a rename that the partition refuses to carry out leaves the file empty rather than
whole, so the rule does not come back at the next boot. A name short enough to be that file
stays taken until it is deleted by hand.

## Missing entities and thermostats

Every entity reference is the `fnv1_hash` of an object id, resolved once at boot. A rule naming
an entity that is not there, or a config switch (`entity_category: config`), stays on disk but
is not built — the log says why and `dump_config` marks it `(not built: <why>)`. Renaming an
entity orphans the rules that used it. A virtual input comes and goes while the device runs, and
takes the rules that name it along, as a thermostat does (below).

Such a file is never rewritten, not even to restamp its `id` or move it to its own name: a file
holds the object ids, memory holds only their hashes, so a rewrite would blank the names. Put
the entity back and the next boot repairs the file as usual.

A rule naming a thermostat, or a preset key, that the hub does not have is refused when it is
added or changed, and at boot it is not built either, its file kept. Thermostats come and go
while the device runs, so the rule follows: it is built once the hub creates that thermostat or
a Save gives it that preset again, and drops out, file kept, when the thermostat is removed or
the preset goes. A rule that builds before and after keeps running. The thermostat's id is kept
as written, so such a file is repaired at boot like any other.

Why a rule is not built, in a sentence the editor shows, is its config's `build_error`:
`Trigger 1: input not found`, `Action 2: thermostat "attic" not found`,
`Else action 1: thermostat "living-room" has no preset "eco"`; `""` when it is built.

## From lambdas

`esphome::global_automation_storage` is the component. The mutators may be called from any task;
they run on the loop task and block the caller. From inside a rule's own action (a switch
callback, say) they refuse and return false.

- `add_automation(config, &error)`: the assigned id, `0` on failure; `error`, when given, says
  why the rule cannot be built, when that is why
- `update_automation(id, config, &error)`, `remove_automation(id)`
- `set_enable_automation(id, enable, persisted = nullptr)`: `persisted` says whether it reached flash
- `reset_all()`: remove every rule and its file
- `run_automation(id)`: play the rule's actions now, skipping its triggers and its condition,
  enabled or not; the mode applies as for a trigger, and a `follow` has no state to copy unless
  a trigger arrives while the run plays. A rule with a `condition` trigger plays the branch its
  condition gives, carrying that result. `RunResult::STARTED`, else `NOT_FOUND`, `NOT_BUILT`,
  `ALREADY_RUNNING` (single with a run under way, parallel at 8 copies) or `REFUSED` (the
  engine failed at boot, the loop never took it, or the call came from inside a rule's own
  action)
- `is_name_taken(name, exclude_id = 0)`: names collide by the file name they spell before the cut
- `check_name(name, &error)`: false for a name that is not UTF-8 or carries a control character;
  `add_automation` refuses such a name with that reason, and so does `update_automation` unless
  the rule already had it
- `rules_naming_binary_sensor(key)`: the names of the rules whose trigger or condition names the
  binary sensor with that object id hash, built or not; loop task
- `rebuild_rules_naming_binary_sensor(key)`: that sensor came or went, so those rules are built or
  dropped to match; loop task
- `configs()`: the loaded `AutomationConfig`s, including the ones that did not build, each with
  its `build_error` and `last_run_ms`: the `uptime_ms()` at which a run with an action to play
  last started, from a trigger that the mode let through or from `run_automation`, unset until
  one has since boot
- `is_running(id)`: a run of the rule is under way
- `run_on_loop(job)`: runs `job` on the loop task and blocks; `false` when the loop never got to it

`configs()`, `is_name_taken()` and `is_running()` read the list where it lives, so they are for
the loop task — a mutator called from a lambda there reallocates it under a reader on any other.
A caller that is not the loop task reads through `run_on_loop`, together with whatever it
decides from the read, so that its answer describes one state of the list.

## Testing

`python tests/run.py automations` builds the engine for the ESPHome `host` platform into a Google
Test binary and runs it, with a directory standing in for the flash; the cases are in
`tests/components/automations/`. `python tests/run.py automations_no_climate` builds it on a
firmware without `climate_hub`. See [doc/TESTING.md](../../doc/TESTING.md).
