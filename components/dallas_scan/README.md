# dallas_scan

DS18B20 temperature sensors found on a 1-Wire bus at boot, one `sensor` entity per device,
none of them declared in YAML. Every device gets a slot, and the slot keeps the device's ROM
address in flash: a sensor keeps its number across reboots and across changes to the other
sensors.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [dallas_scan]

one_wire:
  - platform: ds2484
    id: one_wire_bus

dallas_scan:
  id: temps
  one_wire_id: one_wire_bus
  max_sensors: 16
  update_interval: 20s
```

## Options

| Option            | Default | Meaning                                                                      |
| ----------------- | ------- | ---------------------------------------------------------------------------- |
| `one_wire_id`     |         | The bus to scan                                                              |
| `max_sensors`     | `8`     | Slots, 1-64, and the size of the table in flash; changing it empties the table once |
| `name_prefix`     | `Temp`  | Sensor names are the prefix, a space and the slot number: `Temp 1`           |
| `resolution`      | `12`    | Bits, 9-12, written to the sensors at boot                                   |
| `sensors`         |         | YAML sensors that take the first slots, in this order, see below             |
| `addresses`       |         | Slot number → ROM address, pins the slot to that device                      |
| `filters`         |         | The usual sensor filters, the same chain on every sensor the component creates |
| `update_interval` | `60s`   | One conversion for the whole bus, then one scratch pad read per loop pass    |
| `web_server`      |         | `sorting_group_id` and `sorting_weight`; slot N gets weight + N - 1          |

## Slots

At boot every device that is not in the table takes the lowest free slot, in bus order, and
keeps it. Only Dallas temperature families take a slot (DS18B20, DS18S20, DS1822, DS1825,
DS28EA00); any other 1-Wire device is skipped and logged. An unplugged device keeps its slot
and reads `NaN`; the log notes it once when it stops answering and once when it is back. A
reading of exactly 85.0 °C, the power-on value, is dropped before the filters.

`sensors:` hands the first slots to sensors declared in YAML, `addresses:` pins the others:

```yaml
sensor:
  - platform: dallas_temp
    id: boiler
    name: "Boiler"
    address: 0x8a0122791699dd28
    filters:
      - filter_out: 85.0

dallas_scan:
  sensors: [boiler, pcb_temp]        # slots 1 and 2; any sensor can be listed
  addresses:
    4: 0xeb01227905460228            # slot 4 is always this device, as Temp 4
```

A listed sensor gets no sensor of its own and no reads from the component: it reads its device
itself, with its own name, id, filters and automations, and the component lists it with the
others. A 1-Wire sensor in the list needs an `address:`, which pins the device to that slot so
the scan does not hand it another one. Listed and pinned slots are `pinned(slot)`: forget
leaves them alone.

## Forgetting

`forget(slot)` clears the slot's table entry, saves the table and reboots; the device that was
in it, or a new one, takes the lowest free slot again. `forget(-1)` clears every slot, so the
devices are numbered again in bus order. Pinned slots are skipped, and nothing happens at all
when no slot changes.

## From lambdas

- `sensors()`: the bound slots' sensors, in slot order
- `sensor(slot)`, `temperature(slot)`, `address(slot)`: one slot, 0-based; `nullptr`, `NaN`
  and `0` when the slot is empty
- `pinned(slot)`: fixed by `sensors:` or `addresses:`, so it has no forget entry in a menu
- `max_sensors()`, `forget(slot)`
