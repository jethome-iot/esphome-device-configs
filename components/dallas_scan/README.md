# dallas_scan

DS18B20 temperature sensors found on a 1-Wire bus at boot, one `sensor` entity per device,
created at boot rather than declared in YAML. Every device gets a slot, and the slot keeps the
device's ROM address in flash: a sensor keeps its number across reboots and across changes to
the other sensors.

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
| `one_wire_id`     |         | The bus to scan; may be left out with a single bus                           |
| `max_sensors`     | `8`     | Slots, 1-64, and the size of the table in flash; changing it empties the table once |
| `name_prefix`     | `Temp`  | Sensor names are the prefix, a space and the slot number: `Temp 1`           |
| `resolution`      | `12`    | Bits, 9-12, written to the sensors at boot                                   |
| `sensors`         |         | YAML sensors that take the first slots, in this order, see below             |
| `filters`         |         | The usual sensor filters, the same chain on every sensor the component creates |
| `update_interval` | `60s`   | One conversion for the whole bus, then one scratch pad read per loop pass    |
| `web_server`      |         | `sorting_group_id` and `sorting_weight` of the sensors the component creates; slot N gets weight + N - 1 |

## Slots

At boot every device that is not in the table takes the lowest free slot, in bus order, and
keeps it. Only Dallas temperature families take a slot (DS18B20, DS18S20, DS1822, DS1825,
DS28EA00); any other 1-Wire device is skipped and logged. The bus is scanned at boot only: a
new device shows up after the next reboot, a known one that comes back is picked up at once.
An unplugged device keeps its slot and reads `NaN`; the log notes it once when it stops
answering and once when it is back. A reading of exactly 85.0 °C, the power-on value, is
dropped before the filters.

To fix a slot, or to give a device a sensor of your own with its name, id, filters and
automations, declare the sensor in YAML and list it in `sensors:`: its position in the list is
its slot, and the scan fills the slots after the listed ones. Listed sensors always come
first, so a slot cannot be fixed behind an automatic one. Any sensor can be listed, not only a
1-Wire one, up to `max_sensors` of them:

```yaml
sensor:
  - platform: dallas_temp
    id: boiler
    name: "Boiler"
    address: 0x8a0122791699dd28
    filters:
      - filter_out: 85.0

dallas_scan:
  sensors: [boiler]                  # slot 1
```

A listed slot gets no sensor of its own and no reads from the component: the listed sensor
reads its device itself, with its own name, id, filters and automations, and the component
lists it with the others. A 1-Wire sensor in the list needs an `address:`; its device then
keeps that slot, so the scan does not hand it another one. Listed slots are `pinned(slot)`:
forget leaves them alone.

## Forgetting

`forget(slot)` clears the slot's table entry, saves the table and reboots; the device that was
in it, or a new one, takes the lowest free slot again. `forget(-1)` clears every slot, so the
devices are numbered again in bus order. Listed slots are skipped, and nothing happens at all
when no slot changes. A factory reset (`global_preferences->reset()`) clears the table too.

## From lambdas

Slots are 0-based here.

- `sensors()`: the bound slots' sensors, in slot order
- `sensor(slot)`, `temperature(slot)`, `address(slot)`: `nullptr`, `NaN` and `0` when the slot
  is empty; the address is `0` for a listed sensor that is not a 1-Wire device
- `pinned(slot)`: taken by `sensors:`, so it has no forget entry in a menu
- `max_sensors()`
- `forget(slot)`, `-1` for every slot
