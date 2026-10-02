# mqtt_subscriptions

Reads MQTT topics into entities. A fixed number of slots is set over the device API (the
dashboard's Settings → MQTT → Subscriptions); each enabled slot is one entity, a sensor (Number),
a binary sensor (On/Off) or a text sensor (Text), named after the slot and created when the
device starts. Slot changes apply after a reboot. Needs [`mqtt_config`](../mqtt_config/README.md)
and a filesystem storage. ESP32 only. How it behaves for a user: [doc/MQTT.md](../../doc/MQTT.md).

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [mqtt_subscriptions]

web_server:
  sorting_groups:
    - id: group_mqtt
      name: "MQTT"
      sorting_weight: 35

mqtt_subscriptions:
  id: mqtt_subs
  storage: user_storage
  max_slots: 8
  web_server:
    sorting_group_id: group_mqtt
    sorting_weight: 1
```

## Options

| Option           | Default | Meaning |
| ---------------- | ------- | ------- |
| `id`             |         | The component, for the device API and the display menu |
| `storage`        | required | The filesystem storage the slots are saved on |
| `folder_path`    | `mqtt`  | One folder name below the storage. Not a folder another component keeps on the same storage (`automations`, `crash_report`, `config_json`) |
| `max_slots`      | `8`     | 1 to 16 |
| `units`          | 27 common units (`°C`, `%`, `W`, `kWh`, `ppm`, `µg/m³`, …) | What a Number slot may show: up to 32, each 1 to 16 bytes, none twice |
| `web_server`     |         | The sorting group and weight of the slots' entities; slot N sorts at weight + N − 1 |
| `mqtt_config_id` | the one `mqtt_config` | The MQTT settings whose client it subscribes through |

Every slot takes a place in each of the sensor, binary sensor and text sensor tables, so
`max_slots` is what the firmware reserves.

## The file

The slots are `<storage>/<folder_path>/subscriptions.json`:

```json
{
  "version": 1,
  "slots": [
    {"slot": 1, "enabled": true, "name": "Outdoor temperature", "topic": "zigbee2mqtt/outdoor",
     "kind": "sensor", "json_path": "temperature", "unit": "°C", "decimals": 1,
     "payload_on": "ON", "payload_off": "OFF"}
  ]
}
```

- Only configured slots are listed. `kind` is `sensor`, `binary_sensor` or `text_sensor`.
- A file restored from a backup or written by hand is read again within 30 seconds, and its
  changes, like any other, apply after a reboot. A slot in it that breaks a rule does not run,
  and the device API says why.
- A file that is not valid is renamed to `subscriptions.json.bad` at boot, and the slots start
  empty. One that could not be read just now is left as it is: no slot runs, a save is refused,
  and the next look tries again.
- A file written by newer firmware is left as it is: no slot runs and saves are refused.
- A factory reset formats the storage, so the slots go with it.

## Tests

`tests/components/mqtt_subscriptions/` covers the schema, the shared-folder check and codegen
from Python and, over the harness's `mqtt` stand-in and a directory in place of the storage, the
slot record and its rules, reading payloads, the entities each slot gets, subscribing and
delivering, the crash guard, saving and clearing through the device API, the file and its
mishaps, and slots driving automations and a relay's bound input. LittleFS and esp-mqtt are
ESP-IDF only and out of the host's reach.
