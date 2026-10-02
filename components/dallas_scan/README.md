# dallas_scan

DS18B20 temperature sensors found on a 1-Wire bus at boot, one `sensor` entity per device,
created at boot rather than declared in YAML. Every device gets a slot, and the slot keeps the
device's ROM address in flash, in preferences or in a file: a sensor keeps its number across
reboots and across changes to the other sensors.

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
| `max_sensors`     | `8`     | Slots, 1-64, and the size of the table; changing it empties a table in preferences once |
| `name_prefix`     | `Temp`  | Sensor names are the prefix, a space and the slot number: `Temp 1`           |
| `resolution`      | `12`    | Bits, 9-12, written to the sensors at boot                                   |
| `sensors`         |         | YAML sensors that take the first slots, in this order, see below             |
| `filters`         |         | The usual sensor filters, the same chain on every sensor the component creates |
| `update_interval` | `60s`   | One conversion for the whole bus, then one scratch pad read per loop pass    |
| `web_server`      |         | `sorting_group_id` and `sorting_weight`; slot N gets weight + N - 1. Listed sensors get them too unless they have a `web_server:` block of their own |
| `storage`         | `nvs`   | Where the slot table lives: `nvs` in preferences, `file` in a file on a [`config_json`](../config_json/README.md) mount, see below |
| `config_json_id`  |         | With `storage: file`, the keeper; may be left out with a single `config_json:` |

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

## Storage

With `storage: nvs` the table needs nothing else. With `storage: file` it is
`<config_dir>/dallas_scan_<id>.json` in the keeper's folder, so a copy of that partition carries
the slots along with whatever else is on it:

```json
{"version": 1, "records": [{"slot": 1, "address": "0x8a0122791699dd28"},
                           {"slot": 3, "address": "0xeb01227905460228"}]}
```

The file is read at boot only. It can be edited or restored by hand, then the device rebooted:
a record puts that device in that slot. The address is a string, `0x` and up to 16 hex digits.
A record with a slot past `max_sensors` is skipped and stays in the file until a write, so
lowering `max_sensors` does not empty the table; of two records for the same slot or the same
address, the later one wins; an address that is not a Dallas temperature sensor is dropped.

A file that is there but cannot be read leaves the table empty for that boot and is not written
over: the devices take slots in bus order until the next reboot, and only a forget or an assign
replaces the file. When the partition does not mount, the same happens without a file. The two storages do
not share anything: the first switch to the other one numbers the devices again in bus order,
and switching back finds the table that storage held last.

## Forgetting

`forget(slot)` clears the slot's table entry, saves the table and reboots; the device that was
in it, or a new one, takes the lowest free slot again. `forget(-1)` clears every slot, so the
devices are numbered again in bus order. Listed slots are skipped, and nothing happens at all
when no slot changes, or when the table cannot be written; `can_forget(slot)` says beforehand
whether a slot would change, `can_save()` whether the table can be written.
`global_preferences->reset()` clears a table in preferences; a file goes with its partition.

## Assigning

`assign(slot, address)` puts the device with that ROM into a slot, saves the table and reboots.
A device the table holds in another slot swaps places with what the slot held, so a sensor
moves to another number without reconnecting the sensors one at a time. A new address takes
the slot from its device, which takes the lowest free slot at the next boot if it is still on
the bus; that is also how a sensor gets its number before it is plugged in. A listed slot, a
listed sensor's address, an address that is not a thermometer ROM with a valid CRC, and a
device already in that slot change nothing; `check_assign(slot, address)` says which, first.
Like a forget, an assign that cannot write the table changes nothing.

[`web_device_dashboard`](../web_device_dashboard/README.md) with `dallas_scan_id:` lists the
slots, forgets and assigns them over HTTP, numbered from 1.

## From lambdas

Slots are 0-based here.

- `sensors()`: the bound slots' sensors, in slot order
- `used_slots()`: slots up to the last bound one, free slots between them included, so a
  freed slot keeps its row
- `slot_name(slot)`: the sensor's name, `<prefix> N` for an empty slot
- `sensor(slot)`, `temperature(slot)`, `address(slot)`: `nullptr`, `NaN` and `0` when the slot
  is empty; the address is `0` for a listed sensor that is not a 1-Wire device
- `pinned(slot)`: taken by `sensors:`, so it has no forget entry in a menu
- `max_sensors()`
- `can_forget(slot)`: the slot, or any slot for `-1`, holds a device and is not listed
- `can_save()`: the table can be written; false for a file whose partition did not mount
- `forget(slot)`, `-1` for every slot
- `forget_and_save(slot)`, `assign_and_save(slot, rom)`: the same without the reboot, which
  the caller then owes, and the bus is not read until it comes; false, with the table
  unchanged, when nothing would change or the table could not be written
- `awaiting_reboot()`: one of those wrote the table and the reboot has not come yet; the bus
  is not read until it does
- `valid_address(rom)`: a thermometer family and a valid CRC, the ROMs a slot can hold
- `check_assign(slot, rom)`, `assign(slot, rom)`
