# Testing

Two suites, both built for ESPHome's `host` platform by `esphome compile`, so neither needs an
ESP toolchain. CI runs both in the host-test job.

| Suite | What it is | Run |
| --- | --- | --- |
| `tests/unit/` | Google Test binary over the classes of `components/automations` | `python tests/unit/run.py [--gtest_filter='Runtime.*']` |
| `tests/host/` | the whole engine booted with template entities and a directory as storage, driven by a YAML scenario | `python tests/host/run.py` |

## Which suite

- Parsing, serialization, the enum tables, cron matching, one rule's behavior (edges,
  condition, branches, modes, delays): `tests/unit/`. Seconds per run, one case per behavior.
- Anything that needs the component set up: loading and repairing the rule folder, persisting
  through the API, the second boot, the cron tick against a real clock, the log lines:
  `tests/host/`.
- Only what exists solely on ESP-IDF (NVS, LittleFS, `web_server_base`) has no host test; say so
  in the pull request.

## Adding a unit test

Add a `TEST` or `TEST_F(Runtime, …)` to a file under `tests/unit/cases/`, or a new file there:
the directory is compiled whole. `cases/common.h` provides:

- `entities()`: `in1`, `in2` (binary sensors), `temp` (sensor), `relay1`, `relay2`
  (`FakeSwitch`, which counts its `writes`). Their object ids are `in_1`, `in_2`, `temp`,
  `relay_1`, `relay_2`. `reset_entities()` puts them back; the `Runtime` fixture calls it.
- `FakeEngine`: an `AutomationStorage` whose delays wait in `delays` until `fire_next()`;
  `with_clock()` lets cron triggers build.
- `load(json, config)`, `dump(config)`, `build_rule(engine, json)`.

Rules the binary lives by:

- Nothing declared in `automations-unit.yaml` is set up: `main.cpp` (upstream's
  `tests/components/main.cpp`) runs the tests instead of the generated setup. The YAML only
  pulls the sources in and sets the `USE_*` defines.
- `App` sizes its entity lists from the YAML and silently drops a registration past that. A new
  entity in `common.h` needs a matching declaration in the YAML.
- A test does not touch the filesystem, the scheduler or real time; that is a host-test case.
- Tests live in `esphome::automations::testing`, as upstream's do.

## Adding a host-test case

A rule goes into `tests/host/fixtures/<name>.json`. The scenario in `automations-host.yaml`
publishes states and calls `check` with a label; `run.py` holds what each label must show
(`EXPECTED`), the `[E]` and engine `[W]` lines the run must print and no others, and what the
folder must hold afterwards. A fixture that logs a new error or warning is a change to `run.py`
too. The second pass boots on what the first one stored: `EXPECTED_RESTART`, `EXPECTED_RULES`.
