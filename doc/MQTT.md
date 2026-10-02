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

## Security

- The device side has no access control of its own: anyone who can log in to the web server
  can point the device at another broker. Change the factory `admin` / `admin` login
  ([web_auth](../components/web_auth/README.md)). A web page open in a browser on the same
  network can reach the device through DNS rebinding, which
  [#107](https://github.com/jethome-iot/esphome-device-configs/issues/107) is about.
- Anyone who can publish to the broker can switch the relays. A retained command runs again on
  every connection and overrides the relay's Start mode.
- No TLS: credentials and messages cross the network in the clear.

## Factory reset

A factory reset erases the MQTT settings, and the device starts with MQTT off. With discovery on
and the broker reachable, the device removes its Home Assistant entries first. The retained state
topics and the `offline` on the status topic stay on the broker; without discovery entries Home
Assistant ignores them.

## Repeated crashes

If the device crashes three times in a row while MQTT is connected, for example on a message too
large for it, MQTT is held back: the status says so, and **Reboot now** tries again. A connection
that lasts a minute resets the count.
