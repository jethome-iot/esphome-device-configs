# web_device_dashboard

The device's web UI at `/` — overview, the entities and their settings, automations, the
device log and the file manager — plus the device API it needs under `/api/device/`. The page
is a Vue app built in [jethome-devices-web-dashboard](https://github.com/jethome-iot/jethome-devices-web-dashboard)
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
```

The one option, `board_info_id`, names a `jethome_board_info`; with it `/api/device/info` carries
the identity the firmware read from the CPU board's EEPROM. The handler registers on the shared
`web_server_base` ahead of `web_server`'s, so `/` is the dashboard and `web_server`'s own page is
not reachable; its REST routes, `/events` and its `auth:` stay as they are, and the dashboard uses
them for entity state and control and for the log. The Automations and Files screens talk to
`web_automation_editor` and `web_file_browser` at their default `url_prefix`
(`/automation-editor`, `/files`), baked into the page at build time; without those components the
screens have nothing to show.

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
accumulation and its 4 KiB cap, and the JSON every route answers with. Out of reach there is the
ESP-IDF half — the `Allow` header, URL decoding, the reset reason and the IP lookups, the eFuse
block, and a live WiFi or Ethernet link.
