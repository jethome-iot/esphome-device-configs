# Testing

Every component with tests has a Google Test suite under `tests/components/<name>/`, built for
ESPHome's `host` platform by `esphome compile`, so nothing here needs an ESP toolchain.
`tests/run.py` finds the suites and builds and runs each one; CI does the same in the
`unit-test` job.

```bash
python tests/run.py                                    # every suite
python tests/run.py automations                        # one component
python tests/run.py automations -- --gtest_filter='Storage.*'
```

## Layout

```
tests/
  run.py                    # finds tests/components/*/test.yaml, builds and runs each
  harness/
    main.cpp                # upstream's tests/components/main.cpp: runs the tests instead of setup()
    environment.cpp         # constructs App, which that setup would have done
    components/dir_storage  # test-only storage backend: a directory on the host
  components/
    automations/
      test.yaml             # host config: the component under test, its entities, the harness
      cases/                # the tests; common.h holds what they share
      test_schema.py        # the component's YAML schema, run with unittest by run.py
    i2c_eeprom/             # the same layout, one suite per component
    jethome_board_info/
```

## Adding a suite for a new component

1. `tests/components/<name>/test.yaml`: copy `automations/test.yaml`, keep the `esphome:` block
   (name it `<name>-test`, keep `includes`, `libraries` and the sanitizer flags), replace the
   `external_components` and component sections with the component under test and whatever it
   needs, and declare at least the entities the cases will register.
2. `tests/components/<name>/cases/`: the tests, in `esphome::<name>::testing`, as upstream's.
3. `tests/components/<name>/test_*.py` for the schema: what a bad config is refused with.
4. Nothing else: `run.py` and CI pick the directory up.

## What the automations suite covers

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

Its `cases/common.h` provides:

- `entities()`: `in1`, `in2` (binary sensors), `temp` (sensor), `relay1`, `relay2`
  (`FakeSwitch`, which counts its `writes` and runs `on_change` when the state changes). Their
  object ids are `in_1`, `in_2`, `temp`, `relay_1`, `relay_2`. `reset_entities()` puts them
  back; the fixtures call it.
- `FakeEngine`: an `AutomationStorage` whose delays wait in `delays` until `fire_next()`, whose
  clocks are `ms` (click timing) and `now` (epoch seconds, read by `tick()`), with `with_clock()`
  so cron triggers build, `adopt()` to run a rule inside it and `forget()` to drop the rules
  without touching files.
- `load(json, config)`, `dump(config)`, `build_rule(engine, json)`.
- The `Storage` fixture in `test_storage.cpp`: a temporary directory under `.storage/` next to
  the config, `boot()` / `reboot()`, `write()` / `read()` / `files()`, and `log()` with every
  error and warning logged since the test began.

## What the jethome_board_info suite covers

- `test_crc32.cpp`, `test_header.cpp`, `test_record.cpp`, `test_walk.cpp`, `test_fields.cpp`:
  the JEEFS parser in `jeefs_parse.h` against the format's published vectors, embedded in
  `cases/jeefs_vectors.h` next to builders that assemble headers, file chains and records.
- `test_component.cpp`: the component over an EEPROM image behind a fake `i2c::I2CBus`: a v4
  board with its `device.id`, a v3 board, the headers it refuses, a bus that dies half way, and
  which signature bytes it hands out.

## What the i2c_eeprom suite covers

`test_eeprom.cpp`: how a memory address is framed for each part size, what a write carries,
the ranges it refuses, what a chip that does not answer does to `get`, `put` and `setup`, and
the `on_setup` trigger.

## Rules every suite lives by

- Nothing declared in `test.yaml` is set up: the harness `main.cpp` runs the tests instead of
  the generated setup, and `environment.cpp` constructs `App` because that setup would have.
  The YAML only pulls the sources in and sets the `USE_*` defines.
- `App` sizes its entity lists from the YAML and silently drops a registration past that. A new
  entity in the cases needs a matching declaration in the YAML.
- An engine that subscribed to an entity has to outlive the process: the entity keeps a callback
  into it. The `Storage` fixture keeps its engines for that reason; a test that only needs a
  rule uses `build_rule()`, which never subscribes.
- Nothing runs the scheduler, so a `set_timeout`, `set_interval` or `defer` never fires. The
  seams are `schedule_delay()`, `tick()` and calling `on_startup()` yourself.
- Logger listeners exist only when the YAML asks for them: `test.yaml` carries
  `-DUSE_LOG_LISTENERS -DESPHOME_LOG_MAX_LISTENERS=1` so a suite can read what was logged.
- An I2C component validates on the host only with an `i2c:` bus that names a `device:`;
  nothing opens it. The suite drives the component over a fake `i2c::I2CBus` of its own.
