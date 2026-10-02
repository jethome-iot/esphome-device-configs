# MQTT

Both JXD devices carry an MQTT client. It stays idle until it is set up from the dashboard
(Settings → MQTT), and Home Assistant discovery stays off until it is turned on there. The
native ESPHome API keeps working alongside it either way.

## Setting it up

| Setting | Default | Notes |
| --- | --- | --- |
| Enabled | off | |
| Broker | none | A host name or an IPv4 address, without `mqtt://` and without a port |
| Port | `1883` | |
| Username | none | Up to 64 bytes |
| Password | none | Up to 128 bytes, only with a username. Never shown again once saved; it can be replaced or cleared |
| Client ID | `<node name>-<MAC>` | Empty means the default. Printable ASCII without spaces |
| Topic prefix | `<node name>` | Empty means the default, e.g. `jxd-r6-e1eth-lcd-5c8214`. No `+` or `#`, no leading `$`, no trailing `/` |
| Home Assistant discovery | off | See below |

The dashboard checks the same rules before it saves. The same settings are served at
`/api/device/mqtt` ([openapi.yaml](../components/web_device_dashboard/openapi.yaml)):

```bash
curl --digest -u admin:admin -H 'Content-Type: application/json' \
  -d '{"enabled": true, "broker": "192.168.1.10"}' http://<device>/api/device/mqtt
```

## When a change applies

- The first time MQTT is turned on after the device started, it connects at once, unless the
  effective topic prefix differs from the one the device started with, whether that save or an
  earlier one changed it: then it waits for a reboot.
- Every later change is saved and applies after the next reboot. The dashboard says so and
  offers **Reboot now**. Relays whose Start mode is Off switch off during a reboot, so check
  those first ([ENTITY_SETTINGS.md](ENTITY_SETTINGS.md)).
- Turning Home Assistant discovery off applies at once (see below); turning it on waits for a
  reboot.
- A wrong broker address or password is retried every 10 seconds. The status says why the
  client is not connected when the device can tell: the broker name was not found, the broker
  cannot be reached, the connection was lost, or the broker refused the protocol version, the
  client ID, the service, or the username and password.
- A firmware update keeps the settings. After a rollback to firmware older than the one that
  saved them, MQTT stays off until they are saved again, and the Home Assistant entries the
  newer firmware announced stay on the broker.

## Topics

With the default prefix `jxd-r6-e1eth-lcd-5c8214`:

| Topic | What |
| --- | --- |
| `<prefix>/<domain>/<object id>/state` | Each entity's state, e.g. `jxd-r6-e1eth-lcd-5c8214/switch/relay_1/state` |
| `<prefix>/<domain>/<object id>/command` | Commands, for what can be commanded: Relay 1–6, Red led and, on JetHome's builds, Check for updates |
| `<prefix>/status` | `online` while connected, `offline` otherwise (retained) |

The temperature probes (`Temp N`) publish like any other sensor. There is no log topic and no
`esphome/discover`. The client connects with a clean session, so the broker keeps no
subscriptions of this device between connections.

## Kept off MQTT

Firmware update, Firmware channel, Network mode, and the Modbus address, baud rate, parity and
stop bits are never on MQTT: no state, no command, no discovery. They cut the device off its
network or its field bus, or install firmware. The dashboard, the display and the native API
still have them.

## Home Assistant discovery

Off by default: Home Assistant sees the device through the native API only. Turned on, Home
Assistant also gets the device's entities over MQTT, with MAC-based unique ids and the status
topic as their availability, so each entity shows up a second time.

Turning discovery off removes this device's entries from the broker at once; if the broker
cannot be reached at that moment, they go the next time it connects. A reboot from the dashboard
or the display gives a removal that is going out up to five seconds to finish. An unreachable
broker keeps them in these cases:

- after a broker or port change, the old broker keeps them;
- after MQTT is turned off: the save says so, and turning MQTT on again later with discovery off
  removes them;
- after a factory reset made while the broker was unreachable: Home Assistant keeps the
  entities as unavailable until they are deleted there or on the broker.

## Subscriptions

Settings → MQTT → Subscriptions turns MQTT topics into entities of the device: up to 16
subscriptions, each reading one topic into one entity.

| Field | Notes |
| --- | --- |
| Enabled | A disabled subscription keeps its settings and has no entity |
| Name | The entity's name, up to 32 bytes, without `/`. See "Names" below |
| Topic | One exact topic, up to 128 bytes. No `+` or `#` |
| Kind | Number, On/Off or Text |
| JSON path | Optional: where the value sits in a JSON message, up to 6 keys separated by `.`, e.g. `temperature` or `sensors.0.value`; a number picks an element of a list |
| Unit, Decimals | Number only: a unit from the list the firmware offers, and 0 to 4 decimals (1 by default) |
| Payload on, Payload off | On/Off only: the texts meaning On and Off, `ON` and `OFF` by default, compared ignoring case, up to 32 bytes each |

How a message is read:

- **Number**: a number, also one sent as text. `null`, `none`, `nan`, `unknown` and
  `unavailable` (any case) leave the value unknown, as does an empty message. A JSON `true` or
  `false` reads as 1 or 0.
- **On/Off**: Payload on, then Payload off; failing both, a JSON `true` is On and `false` is Off.
  zigbee2mqtt reports a closed door as `"contact": true`, so for "open is On" set Payload on to
  `false` and Payload off to `true`.
- **Text**: the value as sent, a JSON object or list as JSON, cut to 255 bytes.
- An empty message, such as a cleared retained one, leaves the value unknown.
- A message over 2 KiB is not read.

The dashboard shows each subscription's value, the start of its last message, and why the
message could not be read when it could not: not JSON, the key is not there, not a number,
neither On nor Off, not UTF-8, or over 2 KiB. The same subscriptions are served at
`/api/device/mqtt/subscriptions` ([openapi.yaml](../components/web_device_dashboard/openapi.yaml)),
where each is a numbered slot:

```bash
curl --digest -u admin:admin -H 'Content-Type: application/json' \
  -d '{"slot": 1, "enabled": true, "name": "Outdoor temperature", "topic": "zigbee2mqtt/outdoor",
       "kind": "sensor", "json_path": "temperature", "unit": "°C"}' \
  http://<device>/api/device/mqtt/subscriptions
```

### Names

The entity's id comes from its name: Latin letters, digits, `-` and `_` stay, a space and
everything else become `_`.
Two names giving the same id cannot be used together, so two Cyrillic names of the same length
clash with each other; adding a Latin letter or a digit tells them apart. A subscription cannot
take the id of another subscription, of another entity of the same kind on the device, or of a
temperature probe: `Temp 1` … `Temp 16` are kept for Number subscriptions even while fewer probes
are attached.

Renaming a subscription makes a new entity: Home Assistant and the automations that used the old
one lose it.

### When it applies

- A saved subscription applies after the next reboot, through the same **Reboot now**. A change
  to a subscription that stays disabled needs none.
- Subscriptions that are enabled have their entities whether MQTT is on or not. Turning MQTT on
  starts them reading without a reboot.
- The subscriptions are kept in `/littlefs/mqtt/subscriptions.json`, so a backup carries them
  and a factory reset clears them. A file restored or uploaded there applies after a reboot as
  well.

### The entities

- They sit in the MQTT group on the Entities page and reach Home Assistant through the native
  API. They are never published back over MQTT.
- In automations a Number subscription works as a temperature and an On/Off subscription as an
  input ([AUTOMATIONS.md](AUTOMATIONS.md)). An On/Off subscription can also be a relay's Bound
  input ([ENTITY_SETTINGS.md](ENTITY_SETTINGS.md)): then whoever can publish to that topic
  switches the relay.
- The first value an On/Off subscription gets after a start, usually the retained message, sets
  its state without counting as a press or a release. A Follow binding takes it all the same.

### Topics and large messages

- Exact topics only, at QoS 0. The client connects with a clean session, so what is published
  while the device is offline is lost, except the retained message, which arrives on every
  connect.
- After each connect the subscriptions go out four at a time, so the retained messages of all
  16 arrive within about two seconds; a topic with no retained message holds the ones after it
  back by a second.
- The device takes in every message whole before a subscription reads it, and a very large one,
  tens of kilobytes, can crash it. Pick topics that carry small messages; not
  `zigbee2mqtt/bridge/…`, camera snapshots or `frigate/…`. See "Repeated crashes" for what the
  device does when it happens.

## On the display

The device with the display has an **MQTT** item in its menu, after Automations. Its rows only
show; nothing is set there.

| Row | Shows |
| --- | --- |
| `MQTT: Connected` | Or `Connecting`, `Disconnected`, `Off`, `Not set` (no usable broker saved), `Held back` (see "Repeated crashes") |
| `192.168.1.10` | The broker, with `:<port>` when the port is not 1883; `Broker: --` when none is saved |
| `HA discovery: Off` | Home Assistant discovery |
| `Outdoor: 21.5 °C` | One row per running subscription slot, in slot order; `--` until it has a value. `No subscriptions` when none runs |

- The rows follow the connection and the values as they change, with the menu open.
- They show what runs: a change saved for the next reboot shows after it. While MQTT is off, the
  broker and discovery rows show what is saved.
- A row holds 18 characters, and a longer one is cut with `…`: the broker's name gives way to
  its port, and a slot's name to its value, down to 6 characters.
- The rows draw Latin letters without accents, Cyrillic, digits, the usual signs and `°`, `µ`,
  `²`, `³`. Any other character (`é`, `日`, an emoji), a control character or a byte that is not
  valid UTF-8 shows as `?`.

## Security

- The device side has no access control of its own: anyone who can log in to the web server
  can point the device at another broker. Change the factory `admin` / `admin` login
  ([web_auth](../components/web_auth/README.md)). A web page open in a browser on the same
  network can reach the device through DNS rebinding, which
  [#107](https://github.com/jethome-iot/esphome-device-configs/issues/107) is about.
- Anyone who can publish to the broker can switch the relays and set what the subscriptions
  show. A retained command runs again on every connection and overrides the relay's Start mode.
- No TLS: credentials and messages cross the network in the clear.

## Factory reset

A factory reset erases the MQTT settings, and the device starts with MQTT off. With discovery on
and the broker reachable, the device removes its Home Assistant entries first. The retained state
topics and the `offline` on the status topic stay on the broker; without discovery entries Home
Assistant ignores them.

## Repeated crashes

If the device crashes twice in a row while MQTT is connected, for example on a message too large
for it, it starts with the subscriptions suspended: their entities stay, but nothing is
subscribed, and the dashboard says so. At three crashes MQTT itself is held back. Either way the
status says so and **Reboot now** tries again. A connection that lasts a minute resets the count.
