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
  status page, the menu and the Modbus map read the temperatures through it (`sensors()`,
  `sensor(slot)`, `temperature(slot)`), and `forget_temperatures` (a script) clears slots.
- `display1` and `main_page` come from `display/display.yaml`; the other pages attach with
  `id: !extend display1`. `display_menu` (`display/menu.yaml`) exposes `info_submenu` and
  `menu_settings_id` as extension points that `menu-items-network.yaml` fills via `!extend`;
  `temperatures_menu` gets a `Temp N` submenu per bound slot at boot.
- `${link_icon}` is a substitution holding a C++ expression, defined in `features/network.yaml`
  and expanded inside the main-page lambda in `display/display.yaml`. Package substitutions share
  one namespace with the device config's.
- `display_off_s` (`features/display-off.yaml`) is reset by the `check_blank_page` script
  (`display/display.yaml`), which every button handler runs last.
- `user_storage` (`features/storage.yaml`) is the LittleFS partition mounted at `/littlefs`; code
  that keeps files checks `id(user_storage).is_mounted()` and writes below `get_base_path()`.

## Boot order

`on_boot` blocks are spread across packages and ordered by priority:

| Priority | What runs |
| --- | --- |
| 800 | fill the `relays` / `inputs` vectors |
| 700 | push the stored Modbus address, baud rate, parity and stop bits into `jxm_uart2` |
| 600 | derive the fallback-AP SSID and password from the MAC (`set_wifi_ap`); restore the timezone and read the RTC (`setup_time`, called from the device config). `dallas_scan` sets up at this priority too: after the 1-Wire scan at 999, it binds slots and creates the sensors |
| 500 | add a `Temp N` submenu per bound slot to the Temperatures menu |
| 200 | `apply_network_mode`, then `network_mode_applied = true`; the select's `on_value` is a no-op before that flag, because the restored value fires before the interfaces exist |

## Settings

Template `select` / `number` entities with `optimistic: true` and `restore_value: true`; boot
lambdas read them. Network mode applies live, Modbus settings on the next reboot.

## Temperature slots

The `dallas_scan` component (`components/dallas_scan`, id `temps` in `features/temperature.yaml`)
owns the slots: a slot → ROM address table in flash and one `sensor::Sensor` per bound slot,
`Temp 1` … `Temp 16`, created at setup rather than declared in YAML. `max_sensors` sizes the table
and the entity slots codegen reserves; `sensors:` hands the first slots to YAML sensors and
`addresses:` pins the rest. Adding slots touches
`max_sensors`, `modbus-server.yaml`, the README and `TEMP_COUNT` in `scripts/modbus_probe.py`;
see "More slots" in [ONEWIRE_WORKFLOW.md](ONEWIRE_WORKFLOW.md).

## Modbus

`modbus_server` on `jxm_uart2`. Coils and discrete inputs share one bit table (a bit is a coil iff
it has a `write_lambda`), holding and input registers share one register table, hence inputs sit
at `0x0010`. The map is documented at the top of `features/modbus-server.yaml`; keep
`scripts/modbus_probe.py` and the README in step with it.

## Coupled to upstream internals

The BACK button (`display/buttons.yaml`) reaches `DisplayMenuComponent`'s protected `leave_menu_`
and `finish_editing_` through pointer-to-member casts.

`components/dallas_scan` creates entities at runtime: codegen reserves their places in the
entity tables (`CORE.register_platform_component`) and registers the device class and unit
strings, C++ then calls the four-argument `App.register_sensor` and
`web_server::WebServer::add_entity_config`. The menu rows are `MenuItem`s built by hand.

Re-check both on every ESPHome bump.
