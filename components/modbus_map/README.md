# modbus_map

Names the address ranges a `modbus_server` serves, for
[`web_device_dashboard`](../web_device_dashboard/README.md) to list on its **Settings → Modbus**
tab. The dashboard picks it up on its own; there is nothing to wire.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [modbus_map]

modbus_map:
  modbus_server_id: modbus_server1
  bits:
    - address: 0x0000
      name: Relays
    - address: 0x0010
      name: Inputs
  registers:
    - address: 0x0000
      name: Temperature slots
      scale: 0.1
      unit: "°C"
      no_value: 0x8000
```

An entry names the range that starts at its `address`. Where each range ends, how many values it
holds, whether it is writable and, for registers, the `value_type` all come from the server's own
`bits:` and `registers:`, so they cannot disagree with what the server answers.

| Option | | |
|---|---|---|
| `modbus_server_id` | required | the `modbus_server` the map describes |
| `bits`, `registers` | at least one | the named ranges of each table |
| `address` | required | where the range starts |
| `name` | required | what the range holds |
| `scale` | registers, optional | what one raw unit is worth, e.g. `0.1` |
| `unit` | registers, optional | the unit after scaling |
| `no_value` | registers, optional | the raw word that means "no reading"; one-register values only |

A range is a run of consecutive values that are all writable or all read-only and, for registers,
of one `value_type`. A gap, a change of either, or another entry's address starts the next one.
The build refuses a map that:

- leaves a range unnamed — the message names its addresses and the entry to add;
- names an address the server does not serve, or one inside a multi-register value;
- names one address twice in a table;
- gives `no_value` to a value wider than one register.

With `courtesy_response` enabled on the server, the dashboard also says what an unmapped register
reads.
