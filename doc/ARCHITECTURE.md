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
  `menu_settings_id` as extension points that `menu-items-network.yaml`, `menu-serial.yaml`
  and `menu-firmware.yaml` fill via `!extend` (`firmware_rollback_id`, the instance the
  Rollback row reads, is local to `menu-firmware.yaml`); their rows follow the device config's
  package order unless a `weight` moves them, and rows added from C++ at boot come after all of
  them. A submenu may be empty, and `info_submenu`, `relays_menu` and `inputs_menu` declare no
  rows of their own: the last two are filled at boot from the `relays` / `inputs` vectors, so the
  menu follows whatever the board package put there. `temperatures_menu` gets a `Temp N` submenu per
  slot up to the last bound one at boot; a freed slot's submenu only says `Free slot`.
  `automations_menu` is filled at boot with a row per loaded rule, or one `No automations` row.
- `automations_engine` (`features/automations.yaml`) is the rule engine; `display/menu.yaml`
  reads `configs()` for the Automations rows and calls `set_enable_automation` from them.
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
  `crash_report` (`features/crash-report.yaml`) writes the last panic's record to `crash/` there.
- `config_json_keeper` (`features/storage.yaml`) owns the JSON settings files on that partition
  for any component that registers a settings type with it.
- `switch_settings` and `binary_sensor_settings` (`features/entity-settings.yaml`) are the
  settings objects the menu's Relay N and Input N rows call. Those two ids are set explicitly: a
  generated id cannot be named from a lambda.
- `mqtt_client` and `mqtt_settings` (`features/mqtt.yaml`) are the stock MQTT client and the
  `mqtt_config` component that sets it up from the stored settings. `features/mqtt.yaml` and
  `features/mqtt-firmware.yaml` keep seven entities off MQTT with `state_topic: null` on
  `!extend` of their ids, so those ids are part of the contract: `network_mode`,
  `modbus_address`, `modbus_baud_rate`, `modbus_parity`, `modbus_stop_bits`, and
  `firmware_update` and `firmware_channel` in the package `dist/` leaves out. The menu's Reboot
  device and Factory reset reach `mqtt_config` through `mqtt_config::global_mqtt_config` under
  `#ifdef USE_MQTT_CONFIG`, not `id()`, which a build without the component could not resolve.
- `mqtt_subs` (`features/mqtt-subscriptions.yaml`) is the `mqtt_subscriptions` component, the
  slots that read MQTT topics into entities; it keeps them in `mqtt/` on `user_storage` and
  subscribes through `mqtt_client`. The same package adds the `group_mqtt` sorting group its
  entities sit in.
- `web_auth_credentials` (`features/web-auth.yaml`) holds the credentials the web server checks.
  The `auth:` block in the same file is the factory pair; a pair set through the dashboard is
  kept in the device's flash preferences and replaces it from the next request on, so a factory
  reset from the display menu brings `admin` / `admin` back.
- `web_device_dashboard` (`features/web-device-dashboard.yaml`) is the page at `/`, registered
  ahead of `web_server`'s own. Entity state and control go through `web_server`'s REST and
  `/events`, the Files screen through `web_file_browser` at `/files`, the Automations screen
  through `web_automation_editor` at `/automation-editor`; both prefixes are baked into the page
  at build time and reported at run time by `/api/device/capabilities`. Its `storage_id` is
  `user_storage`, which is what `/api/device/system/factory-reset` wipes — the same wipe the
  menu's Factory reset does.

## Boot order

`on_boot` blocks are spread across packages and ordered by priority:

| Priority | What runs |
| --- | --- |
| 800 | fill the `relays` / `inputs` vectors |
| 700 | push the stored Modbus address, baud rate, parity and stop bits into `jxm_uart2`; build a submenu per entry of those vectors, named after the entity, with its settings rows |
| 600 | derive the fallback-AP SSID and password from the MAC (`set_wifi_ap`); restore the timezone and read the RTC (`setup_time`, called from the device config). `dallas_scan` sets up at this priority too: after the 1-Wire scan at 999, it binds slots and creates the sensors |
| 599 | `automations` sets up: it resolves every rule's entity reference, so it has to stay below the 600 where the `Temp N` sensors are created. `board_info` reads the EEPROM here too, once `eeprom_cpu` (600) has answered |
| 500 | add a `Temp N` submenu per bound slot to the Temperatures menu; add a row per loaded rule to the Automations menu |
| 210 | `mqtt_config` applies the stored MQTT settings: the topic prefix and status topics, discovery, and the broker and credentials when MQTT is on. Ahead of the client's own setup (200) and the entities' MQTT components (`AFTER_CONNECTION`), which build their topics from the prefix; after `dallas_scan` (600), whose `Temp N` it hands to the client |
| 200 | `apply_network_mode`, then `network_mode_applied = true`; the select's `on_value` is a no-op before that flag, because the restored value fires before the interfaces exist |

`littlefs_storage` mounts at 810, so the rule files are readable by the time `automations` loads
them. A wipe a factory reset asked for runs just before that mount. Three things leave the
partition unmounted with the request as it was, so the next boot tries again: a record NVS
cannot be read at all, a wipe that fails, and a wipe that cannot erase its own request
afterwards. An unreadable record is not "no wipe was asked for" — mounting on one serves the
files the reset promised to erase, on a device that came up on its factory credentials —
and mounting on a standing one takes the files written during that boot and erases them at
the next, saying nothing either time.
`crash_report` sets up at 809, right after the mount and ahead of the `config_json` keeper at
`HARDWARE + 5`, and writes the previous boot's crash record there.
Entity settings ride on `setup_priority` instead, ahead of every `on_boot` block: the
`config_json` keeper loads the files at `HARDWARE + 5`, and one apply component per settings type
pushes the values into the entities at `HARDWARE + 1`, before the switches and binary sensors set
themselves up. `bindings` sets up at `DATA`, after every entity, and drives the `Follow` relays
once there; until then input changes are ignored. See [ENTITY_SETTINGS.md](ENTITY_SETTINGS.md).

`mqtt_subscriptions` sets up at `HARDWARE + 3`: after the mount, and before the entity settings
apply at `HARDWARE + 1`, which is what lets a relay's Bound input be a slot's entity. It is also
ahead of `dallas_scan` at `DATA`, so the `Temp N` names are kept free by reservation, since no
probe exists yet, and ahead of the MQTT client, whose `subscribe()` keeps the topics until it
connects. It is the first to ask `mqtt_config` for the crash streak.

## Settings

Template `select` / `number` entities with `optimistic: true` and `restore_value: true`; boot
lambdas read them. Network mode applies live, Modbus settings on the next reboot.

Per-relay and per-input settings are a separate mechanism — JSON files on the user storage
partition rather than preferences — because there is one record per entity and they are meant to
be readable and editable on the partition. See [ENTITY_SETTINGS.md](ENTITY_SETTINGS.md).

## Temperature slots

The `dallas_scan` component (`components/dallas_scan`, id `temps` in `features/temperature.yaml`)
owns the slots: a slot → ROM address table, kept as a `config_json` file on the user partition
(`storage: file`), and one `sensor::Sensor` per bound slot, `Temp 1` … `Temp 16`, created at
setup rather than declared in YAML. `max_sensors` sizes the table
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
  upstream's, carrying `right_for_menu_enter`, the `display_menu.back` action, `fill_row`,
  item `weight` and submenus that may be empty;
  naming them in `external_components` shadows the built-in ones. Every changed hunk is marked
  `JetHome:` and `scripts/vendored-diff.py` prints the whole patch against the pinned ESPHome.
  Dropping the two names from `external_components` builds the upstream components instead.
- `components/web_server_idf` is a copy of upstream's carrying one backported commit, ESPHome
  PR #17800, which is not in the pinned release: a stalled `/events` client had its session
  freed while `esp_http_server` still held the pointer. Every hunk is marked `JetHome:` and
  `scripts/vendored-diff.py` prints the patch. It exists only until the pin catches up —
  dropping `web_server_idf` from `external_components` builds the upstream component instead.
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
  the page it would embed is never served. Its `to_code` also reads the validated config of
  `web_file_browser` and `web_automation_editor` out of `CORE.config` to report their prefixes.
- `components/firmware_rollback`, behind the dashboard's `/api/device/system/rollback` and the
  display's Rollback row, reads otadata the way the bootloader does: the other slot is a target
  only when its entry is one `bootloader_common_ota_select_valid` would boot, so an entry marked
  invalid or aborted, or none at all, is no target. That a half-written slot reads as none is
  ESPHome's OTA backend's doing, which erases the other slot's otadata entry as an update begins.
  That the bootloader then guards the boot is ESPHome's too: `esp32`'s `enable_ota_rollback`
  defaults on wherever `ota:` and `safe_mode` are present, and `safe_mode` marks a boot good only
  when the running firmware is also the boot partition, so the firmware a rollback switched to is
  not confirmed before it has booted.
- `components/web_origin_guard` duplicates `web_server::WebServer::is_request_origin_allowed_`
  rather than calling it: the check is private to a component our handlers do not share, and it
  would not cover `web_server`'s own OTA handler at `/update` in any case. Its catch-all sits in
  front of every handler only because it sets up at `setup_priority::WIFI`, above `web_server`
  and ours at `WIFI - 1` and above the web_server OTA platform at `AFTER_WIFI`, and it writes
  its `403` through `httpd_resp_*` because `AsyncWebServerRequest::send()` maps every status it
  does not know to a 500. That last coupling is `web_device_dashboard`'s and
  `web_file_browser`'s too, and it is why both carry a `send_status_` of their own.
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
- `components/loop_job` reaches into `App.scheduler.set_timeout()` because `Component::defer()`
  is protected and the owner is not the dispatcher, and it takes the scheduler at its word that
  another task may schedule into it.
- `components/bindings` subscribes once per input with `add_full_state_callback` and never
  unsubscribes: upstream has no callback removal, so rebinding goes through its own table.
- `components/crash_report` takes the crash record the one way upstream hands it out, as log
  lines: it checks `esp32::crash_handler_has_data()`, catches what `esp32::crash_handler_log()`
  prints through a logger listener (`logger.request_log_listener()`, `add_log_callback`), then
  calls `esp32::crash_handler_clear()`. The file carries the formatted lines tagged
  `esp32.crash`, cut after the first `]: ` and stripped of colour codes, so the tag, the log
  line layout and `crash_handler_log()`'s wording are part of the report format the decoder
  reads.
- `components/mqtt_config` runs the stock `mqtt` client from settings it stores, and leans on
  how that client works:
  - esp-mqtt takes the broker and credentials when the client first connects and keeps them for
    the boot, so they are pushed once, before the first start; every later change waits for a
    reboot. Command topics are subscribed at the entities' setup with the boot's prefix, so a
    first start waits too once the effective prefix differs from the boot's, whichever save
    changed it. `disable()` does not take an ESP32 client off
    the broker, so nothing calls it.
  - `set_topic_prefix(prefix, check)` takes `prefix` literally unless it equals `check`; an empty
    check value makes a stored prefix literal. The birth, will and shutdown topics are compiled
    as `<CORE.name>/status` and re-targeted at boot to the effective prefix, which carries the
    MAC; final_validate holds the YAML to that.
  - The default client id has no getter: the component rebuilds it with the client
    constructor's formula (node name, `-`, MAC).
  - `state_topic: null` hides an entity because an empty custom state topic makes
    `compute_is_internal_()` true, and `call_setup()` caches that before it registers the
    component. The `Temp N` bridge creates `MQTTSensorComponent`s itself and calls their
    `call_setup()`.
  - Removing Home Assistant entries is the client's own clean mode (`set_discovery_info(…,
    clean=true)`) plus `schedule_resend_state()` on every component; it is finished once the
    client is connected and no component `is_resend_pending()`, which holds because
    `is_connected()` turns true in the same loop pass that schedules the resends.
  - Why a connection failed comes from the ESP32 backend's log lines under the tag `mqtt`,
    `Connection refused error: 0x%x` and `socket errno:`, read through a logger listener; a
    reworded line degrades the reason to "unreachable". `set_on_connect` and
    `set_on_disconnect` add to lists and run on the loop task.
  - `USE_NETWORK_IPV6` is defined with a value, `false` on these builds, so it is tested with
    `#if`, never `#ifdef`.
  - Upstream's default `clean_session: false` would keep removed topics subscribed on the
    broker; final_validate requires `true`. The client reserves the whole announced length of an
    inbound message before it matches the topic, which with exceptions off can abort the device;
    that is what the crash guard counts.
- `components/mqtt_subscriptions` creates its entities at run time the way `dallas_scan` does
  (reserved table places, the four-argument `App.register_*` with hash 0, `add_entity_config`),
  and gives them no MQTT component, so they never reach the broker. Its name check builds object
  ids with `EntityBase`'s per-byte rule (`to_snake_case_char`, then `to_sanitized_char`). An
  On/Off slot is a level on its first value because of `set_trigger_on_initial_state(false)`:
  `StatefulEntityBase` skips the plain callbacks on a first state, also the first after
  `invalidate_state()`, and always calls the full-state ones. It relies on the client calling
  subscription callbacks on the loop task with the whole payload; the client reserves the
  announced length first, which is the abort the crash guard's suspension stage answers.
- `tests/harness/components/mqtt` stands in for upstream's `mqtt` on the host: its schema and
  codegen are upstream's own module, its C++ copies the client's setters, topic prefix rule,
  discovery info and resend pass. A signature drift shows in the ESP32 builds; a behaviour drift
  only by re-reading upstream's `mqtt_client.cpp` and `mqtt_component.cpp`.

Re-check each of these on every ESPHome bump.
