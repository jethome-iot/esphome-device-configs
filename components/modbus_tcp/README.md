# modbus_tcp

Serves a [`modbus_server`](https://esphome.io/components/modbus_server/) over Modbus TCP. Each
request goes to the server's own handlers, so the map, the write lambdas and the exceptions are
the ones its RTU clients get; [`modbus_map`](../modbus_map/README.md) describes both.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [modbus_tcp]

modbus_tcp:
  id: modbus_tcp_server
  modbus_server_id: modbus_server1
  enabled: true
```

| Option | Default | |
|---|---|---|
| `modbus_server_id` | required | the `modbus_server` to serve |
| `enabled` | `false` | whether it listens from boot |
| `port` | `502` | the TCP port |
| `unit_id` | `1` | the unit it answers as, 1–247 |
| `max_clients` | `2` | clients at a time, 1–8; one more is accepted and closed at once |
| `idle_timeout` | `60s` | a client that sends nothing this long is disconnected |
| `serve_wifi_ap` | `false` | answer clients that come through the WiFi fallback access point |

Each client counts towards the sockets ESPHome reserves (`CONFIG_LWIP_MAX_SOCKETS`), as does the
listener.

The server answers function codes 01, 02, 03, 04, 05, 06, 0F, 10 and 17, as the RTU hub does, and
exception `01` to any other. A request to unit 0 or 255 reaches the device as well as one to
`unit_id`; any other unit answers exception `0x0B`. A header whose protocol id is not 0, or whose
length is out of range, closes the connection.

The settings change at run time, from lambdas, and apply in the component's next `loop()`:

```yaml
switch:
  - platform: template
    name: "Modbus TCP"
    optimistic: true
    restore_mode: RESTORE_DEFAULT_OFF
    turn_on_action:
      - lambda: 'id(modbus_tcp_server).set_enabled(true);'
    turn_off_action:
      - lambda: 'id(modbus_tcp_server).set_enabled(false);'
```

`set_port(uint16_t)` and `set_unit_id(uint8_t)` work the same way; a new port closes the clients
and listens again.
