# Testing

`tests/unit/` is a Google Test binary over `components/automations`, built for ESPHome's `host`
platform by `esphome compile`, so it needs no ESP toolchain. CI runs it in the unit-test job.

```bash
python tests/unit/run.py                             # build and run everything
python tests/unit/run.py --gtest_filter='Storage.*'
```

## What goes where

- `test_enums.cpp`, `test_config.cpp`, `test_cron.cpp`: the enum tables, the JSON and the cron
  parsers.
- `test_runtime.cpp`: one rule driven by hand: edges, click timing, condition, branches, modes,
  delays.
- `test_tick.cpp`: the cron tick against a clock the test moves: catch-up and clock jumps.
- `test_storage.cpp`: the whole component over a real directory: loading and repairing the
  folder, refused files, the API and what it writes, the second boot, the errors and warnings it
  logs.
- Only what exists solely on ESP-IDF (NVS, LittleFS, `web_server_base`, the cross-task path of
  the mutators) has no test; say so in the pull request.

## Adding a test

Add a `TEST` or a `TEST_F` to a file under `tests/unit/cases/`, or a new file there: the
directory is compiled whole. `cases/common.h` provides:

- `entities()`: `in1`, `in2` (binary sensors), `temp` (sensor), `relay1`, `relay2`
  (`FakeSwitch`, which counts its `writes`). Their object ids are `in_1`, `in_2`, `temp`,
  `relay_1`, `relay_2`. `reset_entities()` puts them back; the fixtures call it.
- `FakeEngine`: an `AutomationStorage` whose delays wait in `delays` until `fire_next()`, whose
  clocks are `ms` (click timing) and `now` (epoch seconds, read by `tick()`), with `with_clock()`
  so cron triggers build, `adopt()` to run a rule inside it and `forget()` to drop the rules
  without touching files.
- `load(json, config)`, `dump(config)`, `build_rule(engine, json)`.
- The `Storage` fixture in `test_storage.cpp`: a temporary directory under `tests/unit/.storage/`,
  `boot()` / `reboot()`, `write()` / `read()` / `files()`, and `log()` with every error and
  warning logged since the test began.

Rules the binary lives by:

- Nothing declared in `automations-unit.yaml` is set up: `main.cpp` (upstream's
  `tests/components/main.cpp`) runs the tests instead of the generated setup, and
  `cases/environment.cpp` constructs `App` because that setup would have. The YAML only pulls
  the sources in and sets the `USE_*` defines.
- `App` sizes its entity lists from the YAML and silently drops a registration past that. A new
  entity in `common.h` needs a matching declaration in the YAML.
- An engine that subscribed to an entity has to outlive the process: the entity keeps a callback
  into it. The `Storage` fixture keeps its engines for that reason; a test that only needs a
  rule uses `build_rule()`, which never subscribes.
- Nothing runs the scheduler, so a `set_timeout`, `set_interval` or `defer` never fires. The
  seams are `schedule_delay()`, `tick()` and calling `on_startup()` yourself.
- Tests live in `esphome::automations::testing`, as upstream's do.
