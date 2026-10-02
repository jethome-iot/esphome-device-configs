# mqtt_config

Lets the device's MQTT client be set up at run time: the broker, port, credentials, client ID,
topic prefix, Home Assistant discovery and whether MQTT is on at all are stored in the device's
flash and set over the device API (the dashboard's Settings → MQTT). The stock `mqtt:` client is
compiled in idle; this component applies the stored settings when the device starts. ESP32
only. How it behaves for a user: [doc/MQTT.md](../../doc/MQTT.md).

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [mqtt_config]

mqtt:
  id: mqtt_client
  broker: ""
  enable_on_boot: false
  reboot_timeout: 0s
  clean_session: true
  discovery: true
  discovery_retain: true
  discovery_unique_id_generator: mac
  discover_ip: false
  log_topic: null

mqtt_config:
  id: mqtt_settings
```

## Options

| Option           | Default | Meaning |
| ---------------- | ------- | ------- |
| `id`             |         | The component, for the device API and the display menu |
| `mqtt_parent_id` | the one `mqtt:` client | The client it configures |

## What it insists on in `mqtt:`

The device sets these, so a value in YAML would either be overridden or fight it. Each one is
refused with its own message:

| Key | Required | Why |
| --- | --- | --- |
| `broker` | `""` | The broker is set on the device |
| `enable_on_boot` | `false` | The client starts once a broker is set |
| `reboot_timeout` | `0s` | A mistyped broker would otherwise reboot the device every 15 minutes |
| `clean_session` | `true` | A kept session leaves removed topics subscribed on the broker |
| `discovery` | `true` | Discovery is switched on the device; off by default there |
| `discovery_retain` | `true` | Home Assistant loses unretained entries when it restarts |
| `discovery_unique_id_generator` | `mac` | Legacy ids collide between devices |
| `discover_ip` | `false` | The native API already announces the device |
| `log_topic` | `null` | Every log line would go to the broker |
| `wait_for_connection` | `false` | The boot would wait for a broker nobody may have set |
| `topic_prefix`, `client_id` | left out | Set on the device; empty there means the stock default |
| `birth_message`, `will_message`, `shutdown_message` | `topic` left out, or `null` | The status topic is `<topic prefix>/status` of each device; a payload of its own is kept |
| `certificate_authority`, `client_certificate`, `client_certificate_key` | left out | No TLS yet |

## Entities kept off MQTT

With `mqtt:` in the config every entity gets an MQTT component. One that should not be reachable
over MQTT gets `state_topic: null`: no state topic, no command topic and no discovery, while the
native API and the web server keep it. Entities created at run time by `dallas_scan` (the
`Temp N` sensors) get an MQTT component from this one.

## Tests

`tests/components/mqtt_config/` covers the refused `mqtt:` blocks and the entity walk from
Python and, over the harness's `mqtt` stand-in, the stored record and its validation, what a
boot applies, the first start in a boot, the later changes that wait for a reboot, the removal of
Home Assistant entries, the connection states and their causes, the crash guard and the `Temp N`
bridge. NVS, the RTC memory and esp-mqtt itself are ESP-IDF only and out of the host's reach.
