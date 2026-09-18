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
    components: [web_device_dashboard]

web_server:
  port: 80
  version: 3

web_device_dashboard:
  board_info_id: board_info   # optional: the jethome_board_info to report
  storage_id: user_storage    # optional: the mount a factory reset wipes
```

`board_info_id` names a `jethome_board_info`; with it `/api/device/info` carries the identity the
firmware read from the CPU board's EEPROM. `storage_id` names a `filesystem_storage_abstract`
mount: `/api/device/system/factory-reset` wipes it, and `/api/device/capabilities` reports it.
Both are optional and no route disappears without them — `/api/device/info` omits the board
block, and a factory reset clears only the stored settings.

The Files and Automations screens need no option of their own: the component reads the
`url_prefix` of a `web_file_browser` and a `web_automation_editor` off the config and reports
whichever of them this firmware has in `/api/device/capabilities`.

The handler registers on the shared `web_server_base` ahead of `web_server`'s, so `/` is the
dashboard and `web_server`'s own page is not reachable; its REST routes, `/events` and its
`auth:` stay as they are, and the dashboard uses them for entity state and control and for the
log. On a firmware with an `auth:` block the page and every route here are behind it, so the
browser asks for the credentials before the page loads. The page still reaches the Automations
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
| GET | `/api/device/status` | `{"ha_connected", "uptime_s", "reset_reason", "connection_type", "rssi", "ip_address", "reboot_required"}` |
| GET | `/api/device/network` | `{"hostname", "connection_type", "ip_address", "gateway", "subnet", "dns1", "dns2", "ssid", "rssi", "ethernet_connected"}` |
| GET | `/api/device/capabilities` | what this firmware has, below |
| POST | `/api/device/system/reboot` | restart, nothing cleared |
| POST | `/api/device/system/factory-reset` | clear the stored settings and restart, wiping the storage on the way back up (with a `storage_id`) |
| POST | `/api/device/system/rollback` | boot the other app slot — the firmware this one replaced |

### Capabilities

`/api/device/capabilities` answers which screens a client can draw and which routes exist. It
is meant to be read once on load. A key is there only
when the capability is, so the test is `if (caps.files)`; one that has no detail to carry is
`true`. `reboot` and `factory_reset` are
always there, the latter with `clears_storage` — whether a reset also takes the uploaded files
and the automation rules with it. `rollback` names the other app slot and the ESPHome version
of the image in it; that version is what a confirmation dialog should show, because after one
rollback the other slot is the *newer* firmware. It is absent on a board that has never been
updated over the air. `storage`, `files`, `automations`, `entity_settings` and `board_info`
follow the components the firmware was built with. `storage` says what the mount is, not how
full it is: usage is live and this route is read once, so the byte counts stay in the file
API's own `info`.

The embedded page reads this on its **Settings → System** tab and will not draw the tab
without it: a firmware old enough to answer `404` here gets a message saying so rather than
buttons that cannot work. It uses `factory_reset.clears_storage` to say whether a reset takes
the uploaded files with it, and `rollback` to name the slot it would boot — with no key there,
the action stays disabled instead of offering a `503`.

### System actions

All three take `{"confirm": true, "confirm_token": "DD:EE:FF"}` — the last three octets of
`/api/device/info`'s `base_mac_address`, in either case. Not `mac_address`, which on a build
with Ethernet is a different MAC; the `403` says so. Without `confirm` the answer is `400`.
This is a guard against a stray POST from a page the browser happens to load, not an
authorization scheme: whatever reaches the port and can read `/api/device/info` can send it.
Put the routes behind the `web_server:` `auth:` block if that matters.

Each answers before it acts, so the caller gets its answer. A factory reset does what
**Settings → Factory reset** on the display does, in the same order; the files it takes are
gone once the device is back, not when it answers. A rollback selects the
other app slot, checking the image while the request is still open — a slot that is not whole
is a `500` here rather than a device that comes back unchanged — and the firmware it boots gets
one monitored boot: if it fails before it marks itself good, the bootloader returns to this
one. `503` means there is nothing to roll back to.

With a [`web_auth`](../web_auth/README.md), the web server's own credentials; without one
these routes are `404`:

| Method | Path | |
|---|---|---|
| GET | `/api/device/auth` | `{"username", "password_length", "is_default"}` — never the password |
| POST | `/api/device/auth` | `{"username", "password"}` replaces both; needs `Content-Type: application/json`, which no HTML form can send. Answers `200` for a pair it accepted, under the old credentials; the change itself happens on the next turn of the main loop, and the `GET` confirms it |

With a `config_json` store (`entity_config`'s `switch` and `binary_sensor` types), the entity
settings too; without one these routes are `404`:

| Method | Path | |
|---|---|---|
| GET | `/api/device/entities` | per settings type, `[{"source_name", "name"}]`: object id and name of every entity of that type |
| GET | `/api/device/entity-settings?type=switch[&source_name=relay_1]` | the stored records of a type, or one of them |
| POST | `/api/device/entity-settings` | `{"type", "source_name", "settings": {...}}` updates and applies a record; `{"type", "source_name", "action": "delete"}` removes it |
| GET | `/api/device/entity-settings-meta` | the form fields of every settings type |

## client/

`client/device` is the TypeScript client for these routes and `client/rest` the client for
`web_server`'s entity routes and the wire types of its `/events` stream; the dashboard
repository builds against them through its `firmware` link, so they change with the C++ they
mirror. Nothing in this repository builds or type-checks them.

## Tests

`tests/components/web_device_dashboard/` drives the handler on the host through the
`web_server_base` stand-in: the URLs it claims, the route table and its method matrix, the body
accumulation and its 4 KiB cap, the JSON every route answers with, the confirmation the system
actions take, and a factory reset wiping a stand-in storage and the preferences before it
restarts. Out of reach there is the ESP-IDF half — the `Allow` header, URL decoding, the reset
reason and the IP lookups, the eFuse block, a live WiFi or Ethernet link, the real reboot, the
LittleFS format, and the `esp_ota_*` calls behind the rollback, which the tests stand in for.
