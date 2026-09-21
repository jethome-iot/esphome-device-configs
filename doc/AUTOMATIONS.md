# Runtime Automations

Rules that run on the device without recompiling. They live as JSON files on the LittleFS
partition, are loaded at boot and can be created, changed and removed while the device runs:
over HTTP under `/automation-editor/api` ([the routes](../components/web_automation_editor/README.md)),
from a lambda, or by writing into the folder by hand. The file format and the C++ API are in
[components/automations/README.md](../components/automations/README.md).

## What a rule can do

- **Triggers** (any of them fires the rule): input press / release / click / state change,
  switch turn on / off / state change, temperature above / below / in range (fires on the
  crossing, once), cron (six fields, seconds first), startup.
- **Condition** (optional): input is on/off, temperature above / below / in range (`above` and
  `below` are strict, a range includes both ends), and `and` / `or` / `xor` groups of those,
  nested. When false, the `else` actions run.
- **Actions**: switch turn on / off / toggle / follow (copies the state the trigger
  carried, optionally inverted), delay.
- **Mode**: `single` ignores a trigger while the rule is running, `restart` starts over,
  `parallel` runs up to 8 copies.

## Storage

Rules are `/littlefs/automations/<name>.json` on the LittleFS partition from
`features/storage.yaml`, one file per rule, named after the rule. Two rules cannot share a
name. OTA updates keep the files; a factory reset — **Settings → Factory reset → Confirm** on
the display, or `POST /api/device/system/factory-reset` — formats the partition, so the rules
go with it.

Entities are named by object id, so renaming a relay, an input or a temperature sensor leaves
the rules that used it unbuilt — the boot log says which, and their files are kept exactly as
written until the entity is back.

## Over HTTP

`features/automation-editor.yaml` serves the rules on the web server port under
`/automation-editor/api`: the routes and their contract are in
[components/web_automation_editor/openapi.yaml](../components/web_automation_editor/openapi.yaml),
the usage in [its README](../components/web_automation_editor/README.md).

## Testing without hardware

```bash
python tests/run.py automations [-- --gtest_filter='Storage.*']
python tests/run.py web_automation_editor
```

Builds the engine for the host platform into a Google Test binary and runs
`tests/components/automations/cases/`:
the JSON and cron parsers, rules driven by hand with every delay held back until the test fires
it, the cron tick against a clock the test moves, and the whole component over a directory that
stands in for the flash. The second suite drives every HTTP route through the handler, over the
real engine and a stand-in for the web server. How to add a case: [TESTING.md](TESTING.md).
