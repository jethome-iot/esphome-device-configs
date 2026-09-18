# Architecture

How a device config and its packages form one firmware. The tree itself is in the README,
"Repository Layout".

A device config (`devices/<family>/<device>.yaml`) holds substitutions and a `packages:` list,
nothing else. The path substitutions (`assets`, `components`, `boards`, `features`, `display`) are
relative to that file; packages use them as `!include ${features}/…`, `${assets}/fonts/…` and
`source: ${components}`.

ESPHome merges every package into one config, so ids, globals, substitutions and
`esphome.on_boot` entries from different files form one program. The contracts below cross file
boundaries; everything else is local to its file.

## Shared ids

- `web_server.sorting_groups` (`group_relays` … `group_system`) are declared in
  `boards/jxd-cpu-e1eth.yaml`; every visible entity names one.
- `relays`, `inputs` (`boards/jxd-d6-r6-rev1.2.yaml`) are `globals` that the status page, buttons
  and menu iterate over. `temps` (`features/temperature.yaml`) is the `dallas_scan` component; the
  status page, the menu and the Modbus map read the temperatures through it (`used_slots()`,
  `slot_name(slot)`, `sensor(slot)`, `temperature(slot)`), and `forget_temperatures` (a script)
  clears slots.
- `board_info` (`boards/jxd-cpu-e1eth.yaml`) is the `jethome_board_info` component over the
  CPU board's EEPROM `eeprom_cpu`; `display/menu-serial.yaml` reads it for the Serial row and
  `features/web-device-dashboard.yaml` for `/api/device/info`.
- `display1` and `main_page` come from `display/display.yaml`; the other pages attach with
  `id: !extend display1`. `display_menu` (`display/menu.yaml`) exposes `info_submenu` and
  `menu_settings_id` as extension points that `menu-items-network.yaml` and
  `menu-serial.yaml` fill via `!extend`; their rows follow the device config's package order
  unless a `weight` moves them — the Serial rows carry one to stay last under `Info` — and rows
  added from C++ at boot come after all of them. `info_submenu` declares no rows of its own,
  and an empty submenu is allowed: a config that leaves both of those packages out still
  builds and ships an `Info` row that opens nothing, where it used to fail the build.
  `temperatures_menu` gets a `Temp N` submenu per slot up to the last bound one at boot; a
  freed slot's submenu only says `Free slot`.
- `${link_icon}` is a substitution holding a C++ expression, defined in `features/network.yaml`
  and expanded inside the main-page lambda in `display/display.yaml`. Package substitutions share
  one namespace with the device config's.
- `display_off_s` (`features/display-off.yaml`) is reset by the `check_blank_page` script
  (`display/display.yaml`), which every button handler runs last.
- `user_storage` (`features/storage.yaml`) is the LittleFS partition mounted at `/littlefs`; code
  that keeps files checks `id(user_storage).is_mounted()` and writes below `get_base_path()`.
  `web_file_browser` (`features/web-file-browser.yaml`) serves the same mount over HTTP under
  `/files`, on the `web_server` port and with its credentials, so anything written there is also
  reachable from the network. The `automations` component (`features/automations.yaml`) keeps its
  rules there and takes its clock from `pcf8563_time`; `web_automation_editor`
  (`features/automation-editor.yaml`) edits those rules under `/automation-editor/api`.
- `config_json_keeper` (`features/storage.yaml`) owns the JSON settings files on that partition
  for any component that registers a settings type with it.
- `switch_settings` and `binary_sensor_settings` (`features/entity-settings.yaml`) are the
  settings objects the menu's Relay N and Input N rows call. Those two ids are set explicitly: a
  generated id cannot be named from a lambda.
- `web_auth_credentials` (`features/web-auth.yaml`) holds the credentials the web server checks.
  The `auth:` block in the same file is the factory pair; a pair set through the dashboard is
  kept in the device's flash preferences and replaces it from the next request on, so a factory
  reset from the display menu brings `admin` / `admin` back.
- `web_device_dashboard` (`features/web-device-dashboard.yaml`) is the page at `/`, registered
  ahead of `web_server`'s own. Entity state and control go through `web_server`'s REST and
  `/events`, the Files screen through `web_file_browser` at `/files`, the Automations screen
  through `web_automation_editor` at `/automation-editor`; both prefixes are baked into the page
  at build time.

## Boot order

`on_boot` blocks are spread across packages and ordered by priority:

| Priority | What runs |
| --- | --- |
| 800 | fill the `relays` / `inputs` vectors |
| 700 | push the stored Modbus address, baud rate, parity and stop bits into `jxm_uart2`; add the settings rows to the Nth `Relay N` / `Input N` submenu for the Nth entry of those vectors — by position, so a `weight` on one of those submenus would bind its rows to the wrong entity |
| 600 | derive the fallback-AP SSID and password from the MAC (`set_wifi_ap`); restore the timezone and read the RTC (`setup_time`, called from the device config). `dallas_scan` sets up at this priority too: after the 1-Wire scan at 999, it binds slots and creates the sensors |
| 599 | `automations` sets up: it resolves every rule's entity reference, so it has to stay below the 600 where the `Temp N` sensors are created. `board_info` reads the EEPROM here too, once `eeprom_cpu` (600) has answered |
| 500 | add a `Temp N` submenu per bound slot to the Temperatures menu |
| 200 | `apply_network_mode`, then `network_mode_applied = true`; the select's `on_value` is a no-op before that flag, because the restored value fires before the interfaces exist |

`littlefs_storage` mounts at 810, so the rule files are readable by the time `automations` loads
them.
Entity settings ride on `setup_priority` instead, ahead of every `on_boot` block: the
`config_json` keeper loads the files at `HARDWARE + 5`, and one apply component per settings type
pushes the values into the entities at `HARDWARE + 1`, before the switches and binary sensors set
themselves up. `bindings` sets up at `DATA`, after every entity, and drives the `Follow` relays
once there; until then input changes are ignored. See [ENTITY_SETTINGS.md](ENTITY_SETTINGS.md).

## Settings

Template `select` / `number` entities with `optimistic: true` and `restore_value: true`; boot
lambdas read them. Network mode applies live, Modbus settings on the next reboot.

Per-relay and per-input settings are a separate mechanism — JSON files on the user storage
partition rather than preferences — because there is one record per entity and they are meant to
be readable and editable on the partition. See [ENTITY_SETTINGS.md](ENTITY_SETTINGS.md).

## Temperature slots

The `dallas_scan` component (`components/dallas_scan`, id `temps` in `features/temperature.yaml`)
owns the slots: a slot → ROM address table in flash and one `sensor::Sensor` per bound slot,
`Temp 1` … `Temp 16`, created at setup rather than declared in YAML. `max_sensors` sizes the table
and the entity slots codegen reserves; `sensors:` hands the first slots to YAML sensors, the
scan fills the rest. Adding slots touches
`max_sensors`, `modbus-server.yaml`, the README and `TEMP_COUNT` in `scripts/modbus_probe.py`;
see "More slots" in [ONEWIRE_WORKFLOW.md](ONEWIRE_WORKFLOW.md).

## Modbus

`modbus_server` on `jxm_uart2`. Coils and discrete inputs share one bit table (a bit is a coil iff
it has a `write_lambda`), holding and input registers share one register table, hence inputs sit
at `0x0010`. The map is documented at the top of `features/modbus-server.yaml`; keep
`scripts/modbus_probe.py` and the README in step with it.

## Coupled to upstream internals

- `components/display_menu_base` and `components/graphical_display_menu` are copies of
  upstream's, carrying `right_for_menu_enter`, the `display_menu.back` action, `fill_row`, the
  `weight` that sorts each `items:` list during validation, and submenus that may be empty;
  naming them in `external_components` shadows the built-in ones. Every changed hunk is marked
  `JetHome:` and `scripts/vendored-diff.py` prints the whole patch against the pinned ESPHome.
  Dropping the two names from `external_components` builds the upstream components instead.
- `components/dallas_scan` creates entities at runtime: codegen reserves their places in the
  entity tables (`CORE.register_platform_component`) and registers the device class and unit
  strings, C++ then calls the four-argument `App.register_sensor` and
  `web_server::WebServer::add_entity_config`. The menu rows are `MenuItem`s built by hand.
- Upstream builds ESP-IDF with `CONFIG_VFS_SUPPORT_DIR` off, so `components/littlefs_storage`
  calls `esp32.require_vfs_dir()` to keep `opendir`/`mkdir` from being stubs.
- `components/web_file_browser` sits on web-server internals: `/download` writes straight to
  `esp_http_server` through `AsyncWebServerRequest`'s `httpd_req_t *` conversion, `/upload` takes the
  multipart reader's two `handleUpload()` calls at index 0 as the start of a transfer, and that
  multipart branch exists at all only because `ota: - platform: web_server` defines
  `USE_WEBSERVER_OTA` — which a final-validate check in the component insists on.
- `components/web_device_dashboard` owns `/` only by registering first: it sets up at
  `setup_priority::WIFI - 0.5`, just ahead of `web_server`'s `WIFI - 1`, and `web_server_base`
  asks its handlers in registration order. `web_server` therefore runs without `local: true`:
  the page it would embed is never served.
- `components/web_auth` replaces the two `const char *` upstream's `WebServerBase` keeps and
  never copies, so the strings it hands over must outlive every request and the setters are
  called again after each change. It also needs a compiled `auth:` block to exist at all:
  `add_handler()` decides once, at registration, whether a handler gets the authentication
  middleware, and without credentials at that moment none is installed. A final-validate check
  in the component insists on the block.
- `components/virtual_display` (emulator only, see [QEMU.md](QEMU.md)) renders through
  `DisplayBuffer`'s protected `init_internal_` / `do_update_` and serves its endpoints as a
  `web_server_base` handler, setting the 405 status line through ESP-IDF's
  `httpd_resp_set_status` because the IDF response layer maps no such code.
- `components/automations` names entities by `fnv1_hash` of their object id and walks
  `App.get_binary_sensors()` / `get_sensors()` / `get_switches()` itself, so the hash and
  `EntityBase::get_object_id_to` are part of the on-disk rule format.
- `tests/harness/main.cpp` is upstream's `tests/components/main.cpp`: the writer keeps what is
  outside its marker comments and puts the generated setup code into `original_setup()`, which
  is never called. `App` sizes its entity lists from the YAML and drops a registration past
  that, so each `tests/components/<name>/test.yaml` declares at least what its cases register.
- `components/entity_config` force-defines `USE_BINARY_SENSOR_FILTER` so the filter chain compiles
  without YAML filters, appends a `binary_sensor::Filter` at run time, and keys every stored
  record on `fnv1_hash(object_id) == EntityBase::get_object_id_hash()`, an equality upstream does
  not promise.
- `components/config_base` schedules its debounced save with a named string timeout and flushes
  from `on_shutdown()`.
- `components/bindings` subscribes once per input with `add_full_state_callback` and never
  unsubscribes: upstream has no callback removal, so rebinding goes through its own table.

Re-check each of these on every ESPHome bump.
