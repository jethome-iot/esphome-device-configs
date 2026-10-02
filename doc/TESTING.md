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
  run.py                    # finds the suites under tests/components/, builds and runs each
  harness/
    main.cpp                # upstream's tests/components/main.cpp: runs the tests instead of setup()
    environment.cpp         # constructs App, which that setup would have done
    entity_tables.h         # puts App's entity lists back to a length, for components that
                            # register entities at setup
    components/
      dir_storage           # test-only storage backend: a directory on the host
      display_menu_host     # test-only key that pulls display_menu_base into a host build
      loop_job_host         # the same for loop_job
      mqtt                  # stand-in for upstream's, which builds for ESP platforms only: a client
                            # and entity components a test drives in place of a broker
      one_wire_host         # test-only 1-Wire bus: the cases set what the boot scan found
      web_server            # stand-in for upstream's, which builds for ESP platforms only
      web_server_base       # stand-in for upstream's, so HTTP handlers run on the host
  components/
    automations/
      test.yaml             # host config: the component under test, its entities, the harness
      cases/                # the tests; common.h holds what they share
      test_schema.py        # the component's YAML schema, run with unittest by run.py
    bindings/               # the same layout, one suite per component
    config_json/
    crash_report/
    dallas_scan/
    display_menu_base/
    entity_config/
    firmware_rollback/
    i2c_eeprom/
    jethome_board_info/
    jethome_manifest/
    jethome_update/           # test_schema.py alone: the entity needs ota.http_request, which
                              # upstream refuses on the host platform
    littlefs_storage/         # test_schema.py alone: the C++ is ESP-IDF only
    loop_job/
    mqtt_config/
    mqtt_subscriptions/
    virtual_display/          # test_schema.py alone: the C++ includes <esp_http_server.h>,
                              # which the host platform has no header for
    web_auth/
    web_automation_editor/
    web_device_dashboard/
    web_file_browser/
    web_origin_guard/
```

## Adding a suite for a new component

1. `tests/components/<name>/test.yaml`: copy `automations/test.yaml`, keep the `esphome:` block
   (name it `<name>-test`, keep `includes`, `libraries` and the sanitizer flags), replace the
   `external_components` and component sections with the component under test and whatever it
   needs, and declare at least the entities the cases will register.
2. `tests/components/<name>/cases/`: the tests, in `esphome::<name>::testing`, as upstream's.
3. `tests/components/<name>/test_*.py` for the schema: what a bad config is refused with.
4. Nothing else: `run.py` and CI pick the directory up. A component the host platform cannot
   build gets a directory with `test_*.py` and no `test.yaml`; the runner then skips the build.

## Rules every suite lives by

- Nothing declared in `test.yaml` is set up: the harness `main.cpp` runs the tests instead of
  the generated setup, and `environment.cpp` constructs `App` because that setup would have.
  The YAML only pulls the sources in and sets the `USE_*` defines.
- `App` sizes its entity lists from the YAML and silently drops a registration past that. A new
  entity in the cases needs a matching declaration in the YAML. A component that creates its
  entities at setup is booted again and again in one process, so its suite puts the lists back
  to their length before each boot with `harness/entity_tables.h`, listed in its `includes:`.
- A component that subscribed to an entity has to outlive the process: the entity keeps a
  callback into it. Keep such objects alive across tests instead of destroying them.
- Nothing runs the scheduler, so a `set_timeout`, `set_interval` or `defer` never fires unless a
  test calls `App.scheduler.call(millis())` itself after letting the clock advance, as the
  config_json debounce test does. Otherwise a component gives the tests a seam instead: a virtual
  they override, or a step they call.
- Logger listeners exist only when the YAML asks for them: `test.yaml` carries
  `-DUSE_LOG_LISTENERS -DESPHOME_LOG_MAX_LISTENERS=1` so a suite can read what was logged. A
  component that requests a listener slot of its own (`crash_report`, `mqtt_config`) gets them
  from codegen instead, and the hand-set flags would clash with it. A suite that has such a
  component and reads the log itself sizes a slot of its own with a `logger: on_message:`
  trigger, as `crash_report`'s does; the list drops a listener past its size without a word.
- The host preferences keep no record over 255 bytes. A component that stores a larger one
  (`mqtt_config`) reads and writes it through a virtual seam the tests override.
- A suite that needs `mqtt:` loads the `mqtt` stand-in from `tests/harness/components`. Every
  entity then gets a stand-in MQTT component, as on a device; `connect_for_test()` (or its two
  halves, `backend_connect_for_test()` and `take_connection_for_test()`), `drop_for_test()`,
  `deliver_for_test()` and `process_resends_for_test()` on the client play the broker, and
  `published` and `sent_subscribes` hold what went out.
- An I2C component validates on the host only with an `i2c:` bus that names a `device:`;
  nothing opens it. The suite drives the component over a fake `i2c::I2CBus` of its own.
- A 1-Wire component needs `one_wire: - platform: one_wire_host` from the harness: upstream's
  `gpio` bus compiles on the host but does not link. The case sets the devices the boot scan
  found with `set_devices()`; every device answers all ones, so no scratch pad passes its
  checksum and a reading is never published.
- Entity strings (units, device classes, icons) are indices into tables codegen builds from the
  YAML: declare the unit in `test.yaml` and pass its index in `entity_fields`.
