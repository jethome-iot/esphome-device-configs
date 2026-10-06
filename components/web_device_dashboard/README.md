# web_device_dashboard

The device's web UI at `/` — overview, the entities and their settings, automations, the
device log, the file manager and the device settings — plus the device API it needs under
`/api/device/`. The page is a Vue app built in
[jethome-devices-web-dashboard](https://github.com/jethome-iot/jethome-devices-web-dashboard)
and embedded here, gzipped, as `dashboard_index.h`, so a device config needs no Node.js.
ESP-IDF only.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [web_device_dashboard, web_origin_guard, loop_job, firmware_rollback]

web_server:
  port: 80
  version: 3

web_device_dashboard:
  board_info_id: board_info   # optional: the jethome_board_info to report
  storage_id: user_storage    # optional: the mount a factory reset wipes
  dallas_scan_id: temps       # optional: the temperature slots, their offsets and labels
```

`board_info_id` names a `jethome_board_info`; with it `/api/device/info` carries the identity the
firmware read from the CPU board's EEPROM. `storage_id` names a `filesystem_storage_abstract`
mount: `/api/device/system/factory-reset` wipes it, and `/api/device/capabilities` reports it.
Both are optional and no route disappears without them — `/api/device/info` omits the board
block, and a factory reset clears only the stored settings. `dallas_scan_id` names a
[`dallas_scan`](../dallas_scan/README.md); without it the temperature-slot routes are `404`.

The Files, Automations and Climate screens need no option of their own: the component reads
the `url_prefix` of a `web_file_browser`, a `web_automation_editor` and a `web_climate_editor`
off the config and reports whichever of them this firmware has in `/api/device/capabilities`.
A [`modbus_map`](../modbus_map/README.md) is picked up the same way.

The **Settings → Network** and **Settings → Modbus** tabs show the entities of the `web_server`
sorting groups named `Network` and `Modbus`, and the Entities screen leaves those groups out. The
Entities screen also leaves out `Firmware channel` and `Check for updates` while
**Settings → Firmware** shows them. The Modbus tab is there when its group has an entity or the
firmware has a `modbus_map`, which the tab lists beside the settings.

The handler registers on the shared `web_server_base` ahead of `web_server`'s, so `/` is the
dashboard and `web_server`'s own page is not reachable; its REST routes, `/events` and its
`auth:` stay as they are, and the dashboard uses them for entity state and control and for the
log. On a firmware with an `auth:` block the page and every route here are behind it, so the
browser asks for the credentials before the page loads, and
[`web_origin_guard`](../web_origin_guard/README.md) answers `403` to a request whose `Origin` is
not the `Host` it was sent to, so those credentials cannot be steered here by a page on another
site. The page still reaches the Automations
and Files screens at the prefixes baked into it at build time (`/automation-editor`, `/files`),
and shows them whether or not the firmware serves them.

## Updating the page

`dashboard_index.h` is generated, never edit it. `npm run build` in the dashboard repository
writes `dist/dashboard_index.h`: copy it here and commit. That repository's Release workflow,
run by hand, opens the same change as a pull request.

## REST

Reads answer GET and writes POST, `entity-settings` both; the wrong method is `405` with an
`Allow` header, an unknown route under `/api/device/` `404`, a body over 4 KiB `413`, and every
failure `{"success": false, "error"}`. The same contract, machine-readable:
[openapi.yaml](openapi.yaml) (OpenAPI 3.1).

| Method | Path | |
|---|---|---|
| GET | `/api/device/info` | `{"name", "base_mac_address", "mac_address", "version"}`; with `board_info_id` also `serial_number`, `device_model`, `hw_revision` and `board` — what `jethome_board_info` read, verbatim, plus the chip's eFuses |
| GET | `/api/device/status` | `{"ha_connected", "uptime_s", "reset_reason", "connection_type", "rssi", "ip_address", "reboot_required", "reboot_reasons"}`: `reboot_reasons` names what waits for a restart — `temperature_slots` after a forget or an assign — and is absent when nothing does |
| GET | `/api/device/network` | `{"hostname", "connection_type", "ip_address", "gateway", "subnet", "dns1", "dns2", "ssid", "rssi", "ethernet_connected"}` |
| GET | `/api/device/capabilities` | what this firmware has, below |
| POST | `/api/device/system/reboot` | restart, nothing cleared; what waits for a restart applies |
| POST | `/api/device/system/factory-reset` | clear the stored settings and restart, wiping the storage on the way back up (with a `storage_id`) |
| POST | `/api/device/system/rollback` | boot the other app slot — after an update, the firmware it replaced |

### Capabilities

`/api/device/capabilities` answers which screens a client can draw and which routes exist. It is
meant to be read on load, not polled. A key is there only when the capability is, so the test is
`if (caps.files)`; one that has no detail to carry is `true`. `reboot` and `factory_reset` are
always there, the latter with `clears_storage` — whether a reset also takes the uploaded files and
the automation rules with it. `rollback` names the other app slot and the ESPHome version of the
image in it; that version is what a confirmation dialog should show, because after one rollback the
other slot is the *newer* firmware. It is there when
[`firmware_rollback`](../firmware_rollback/README.md) finds a firmware to go back to, and absent
after a serial flash, a failed or interrupted update, a rollback the bootloader did itself, or while
a switch waits for its reboot. `storage`, `files`, `automations`, `climates`, `entity_settings`,
`board_info`, `temperature_slots` and `modbus` follow the components the firmware was built with.
`storage` says what the mount is, not how full it is: usage is live and this route is not polled,
so the byte counts stay in the file API's own `info`. `modbus` is the Modbus server's address map
as a `modbus_map` names it: the bit and register ranges, each with its first and last address, how
many values it holds, whether it is writable and its name, a register range also with its
`value_type` and, where the map gives them, `scale`, `unit` and `no_value`; and, when the server
has one enabled, the `courtesy_response` an unmapped register gets.

The embedded page will not draw its **Settings → System** tab without this: a firmware old
enough to answer `404` here gets a message saying so rather than buttons that cannot work. It
uses `factory_reset.clears_storage` to say whether a reset takes the uploaded files with it.
Its **Settings → Firmware** tab uses `rollback` to name the slot a rollback would boot — with
no key there, the action stays disabled instead of offering a `503`. Since that key moves, the
page reads this route again whenever it shows the rollback, and after an update fails or a
rollback is refused, rather than only on load. Its **Settings → Temperature** tab is gone once
this route answers without `temperature_slots`.

### System actions

All three take `{"confirm": true, "confirm_token": "DD:EE:FF"}` — the last three octets of
`/api/device/info`'s `base_mac_address`, in either case. Not `mac_address`, which on a build
with Ethernet is a different MAC; the `403` says so. Without `confirm` the answer is `400`, and
without `Content-Type: application/json` a `415`.
The token is not a secret. It is derived from the MAC, which also ends the device's hostname
when the configuration sets `name_add_mac_suffix`, and mDNS publishes it besides — anyone who
can address the device can spell it. It is there so a single stray POST does not reboot a
device; what keeps other sites out is the `web_server:` `auth:` block and
[`web_origin_guard`](../web_origin_guard/README.md).

Each answers before it acts, so the caller gets its answer. A factory reset does what
**Settings → Factory reset** on the display does, in the same order; the files it takes are
gone once the device is back, not when it answers. A rollback selects the
other app slot, checking the image while the request is still open — a slot that is not whole
is a `500` here rather than a device that comes back unchanged — and the firmware it boots gets
one monitored boot: if it fails before it marks itself good, the bootloader returns to this
one. `503` means there is nothing to roll back to, or that the device was too busy to take the
request (`Device busy`) and selected nothing.

With a [`web_auth`](../web_auth/README.md), the web server's own credentials; without one
these routes are `404`:

| Method | Path | |
|---|---|---|
| GET | `/api/device/auth` | `{"username", "password_length", "is_default"}` — never the password |
| POST | `/api/device/auth` | `{"username", "password"}` replaces both; needs `Content-Type: application/json`, which no HTML form can send. Answers `200` for a pair it accepted, under the old credentials; the change itself happens on the next turn of the main loop, and the `GET` confirms it |

With a `dallas_scan_id`, the temperature slots; without one these routes are `404`:

| Method | Path | |
|---|---|---|
| GET | `/api/device/temperature-slots` | `{"max_slots", "reboot_required", "can_forget_all", "max_offset", "offset_step", "max_label_length", "slots": [{"slot", "name", "free", "listed", "address", "can_forget", "pending", "running_address", "offset", "label"}]}`: slots 1 up to the last one bound at boot, held in the saved table or holding an offset or a label, a free slot between them included. `free`, `address` and `can_forget` describe the saved table; `pending` marks a slot that differs from boot, and `running_address` is the ROM its sensor reads until the reboot. `address` is the ROM as a hex string, absent for a free slot and a listed sensor that is not a 1-Wire one. `offset` is the slot's offset in °C, in force already; absent on a listed slot. `label` is what the panel and the page show in place of `name`, `""` for none; absent on a listed slot. `max_label_length` (24) and the rows' `label` are there only with `dallas_scan`'s `storage: file`, which keeps labels. `can_forget_all` says whether a forget of every slot would change anything — a device, an offset or a label on an unlisted slot, and a table that can be written. `503` when the loop task does not take the read |
| POST | `/api/device/temperature-slots/forget` | `{"slot": N}` empties that slot, `{"all": true}` every slot but the listed ones — what the panel's forget rows do, without the restart: the change is saved and applies after a reboot. `all` also clears every offset and label, at once; one slot keeps its own. Takes the system actions' confirmation; a request that would change nothing — a free or listed slot, or nothing to forget at all — is `409`; a table that cannot be written is `503`, a write that fails `500`; `503` too when the loop task does not take it |
| POST | `/api/device/temperature-slots/assign` | `{"slot": N, "address": "0x…"}` puts that device into slot N, applied after a reboot. A device already in another slot swaps with what slot N held; a new address takes slot N from its device, which takes the lowest free slot at the next boot if it is still on the bus. Same confirmation; an address that is not a thermometer ROM with a valid CRC is `400`, a listed slot or device, or a device already there, `409`, a table that cannot be written `503`, a write that fails `500`, a loop task that does not take it `503` |
| POST | `/api/device/temperature-slots/offset` | `{"slot": N, "offset": x}` sets slot N's offset, -5.0 to +5.0 °C, rounded to 0.1; `0` removes it. Saved and in force at once, with no confirmation and no reboot: the slot's reading is published again with it. A free slot takes one for the sensor that takes it later. A slot out of range or an offset that is not a number within the range is `400`, a listed slot `409`, a table that cannot be written or a slot file that did not load at boot `503`, a write that fails `500`, a loop task that does not take it `503` |
| POST | `/api/device/temperature-slots/label` | `{"slot": N, "label": "text"}` sets slot N's label, trimmed of the spaces at both ends; `""` clears it. At most 24 characters, well-formed UTF-8 with no control character, as a relay's label. Saved and shown at once, with no confirmation and no reboot; Home Assistant, Modbus, the rules and the thermostats keep `Temp N`. A free slot takes one for the sensor that takes it later. Without `storage: file` it is `404`; a slot out of range or a label the rules refuse is `400`, a listed slot `409`, a table that cannot be written or a slot file that did not load at boot `503`, a write that fails `500`, a loop task that does not take it `503` |

Slots are numbered from 1, as the `Temp N` sensors are. A forget or an assign answers
`{"success", "message", "reboot_required"}`; changes add up until a reboot applies them all, and
one that puts the table back as the device booted leaves nothing waiting. An offset answers
`{"success", "message", "offset"}`, the offset the slot now holds, and a label
`{"success", "message", "label"}`, the label as stored. A slot's reading is
not here: it is the state of the sensor of that `name` on `web_server`'s `/events`.

With a `config_json` store (`entity_config`'s `switch` and `binary_sensor` types), the entity
settings too; without one these routes are `404`:

| Method | Path | |
|---|---|---|
| GET | `/api/device/entities` | per settings type, `[{"source_name", "name", "label"}]`: object id, name and label of every entity of that type, the label empty when it has none. Read on the loop task, which owns the records — `503` when it does not get to it |
| GET | `/api/device/entity-settings?type=switch[&source_name=relay_1]` | the stored records of a type, or one of them. Read on the loop task, which owns the records — `503` when it does not get to it |
| POST | `/api/device/entity-settings` | `{"type", "source_name", "settings": {...}}` updates and applies a record; `{"type", "source_name", "action": "delete"}` removes it. A change of a relay's `inverted` while a running thermostat drives it is `409`, naming the thermostat. Needs `Content-Type: application/json`, as every route here that reads a body does |
| GET | `/api/device/entity-settings-meta` | the form fields of every settings type |

Both types take a `label`, which the panel and the page show in place of the entity's name: a
string of at most 24 characters (code points), valid UTF-8 with no control character, trimmed
of the spaces at both ends; empty clears it. The meta describes it as a `string` field with
`max_length: 24`. A bad one is the `400` `Failed to update settings record`, and an update that
leaves it out keeps it. The name, the object id and `web_server`'s routes do not change.

## client/

`client/device` is the TypeScript client for these routes and `client/rest` the client for
`web_server`'s entity routes and the wire types of its `/events` stream; the dashboard
repository builds against them through its `firmware` link, so they change with the C++ they
mirror. Nothing in this repository builds or type-checks them.

## Tests

`tests/components/web_device_dashboard/` drives the handler on the host through the
`web_server_base` stand-in: the URLs it claims, the route table and its method matrix, the body
accumulation and its 4 KiB cap, the JSON every route answers with, the confirmation the system
actions take, a factory reset wiping a stand-in storage and the preferences before it restarts,
and the temperature slots listed, forgotten and assigned and their offsets and labels set on a real
`dallas_scan` over the harness's 1-Wire bus, with `/status` reporting what waits, booted again to
see what the table kept. Out of reach there is the ESP-IDF half — the `Allow` header, URL
decoding, the reset reason and the IP lookups, the eFuse block, a live WiFi or Ethernet link, the
real reboot, the LittleFS format, and the rollback's reads and switch, which the tests stand in
for; the rule that decides is covered by [`firmware_rollback`](../firmware_rollback/README.md)'s
own suite.
