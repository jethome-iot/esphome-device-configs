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
| `max_sensors`     | `8`     | Slots, 1-64, and the size of the table; changing it empties a table and its offsets in preferences once |
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
dropped before the slot's offset and the filters.

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

With `storage: nvs` the table and the offsets need nothing else. With `storage: file` they are
`<config_dir>/dallas_scan_<id>.json` in the keeper's folder, so a copy of that partition carries
the slots along with whatever else is on it, and the file holds the slots' [labels](#labels) too:

```json
{"version": 1, "records": [{"slot": 1, "address": "0x8a0122791699dd28"},
                           {"slot": 3, "address": "0xeb01227905460228"}],
               "offsets": [{"slot": 3, "offset": -0.3}],
               "labels": [{"slot": 3, "label": "Подача"}]}
```

The file is read at boot only. It can be edited or restored by hand, then the device rebooted:
a record puts that device in that slot. The address is a string, `0x` and up to 16 hex digits.
A record with a slot past `max_sensors` is skipped and stays in the file until a write, so
lowering `max_sensors` does not empty the table; of two records for the same slot or the same
address, the later one wins; an address that is not a Dallas temperature sensor is dropped.
`offsets` lists only the slots with an offset, in °C, and may be left out; an entry with a slot
past `max_sensors` or a value that is not a number within ±5.0 is skipped, of two entries for
the same slot the later one wins, and a value is rounded to 0.1. An `offsets` that is not a list
is skipped and the records still load. Firmware older than offsets reads the records, ignores
the list and drops it the next time it writes the file.
`labels` lists only the slots with a label and may be left out too; an entry with a slot past
`max_sensors`, or a label that is not a string or breaks the [rules](#labels), is skipped, of two
entries for the same slot the later one wins, and the spaces at both ends of a label are
trimmed. A `labels` that is not a list is skipped and the rest still loads. Firmware older than
labels, a rollback target included, ignores the list and drops it the next time it writes the
file: at a forget, an assign, an offset, or a boot that binds a new device.

A file that is there but cannot be read leaves the table empty for that boot and is not written
over: the devices take slots in bus order until the next reboot, only a forget or an assign
replaces the file, and no offset or label can be set until it loads. When the partition does not
mount, the same happens without a file. The two storages do not share anything: the first switch
to the other one numbers the devices again in bus order, and switching back finds the table that
storage held last.

## Forgetting

`forget(slot)` clears the slot's table entry, saves the table and reboots; the device that was
in it, or a new one, takes the lowest free slot again, and the slot keeps its offset and label.
`forget(-1)` clears every slot, every offset and every label, so the devices are numbered again
in bus order and no offset or label lands on another sensor; with no device left in the table, it
still clears the offsets and labels, in the same write. Listed slots are skipped, and nothing
happens at all when nothing would change (for `-1`: no device, offset or label left), or when the
table cannot be written; `can_forget(slot)` says beforehand whether anything would change,
`can_save()` whether the table can be written. With
`storage: nvs`, a `forget(-1)` whose offsets are written but whose table is not keeps the offsets
cleared and reports the failure, the slots unchanged. While a change
waits for a reboot (below), `forget(slot)` on a slot changed since boot, and `forget(-1)` with
nothing left to forget, only reboot when the table can be written: the saved table applies as
it is.
A factory reset clears the table, the offsets and the labels with either storage:
`global_preferences->reset()` empties preferences, and a file goes with its partition.

## Assigning

`assign(slot, address)` puts the device with that ROM into a slot, saves the table and reboots.
A device the table holds in another slot swaps places with what the slot held, so a sensor
moves to another number without reconnecting the sensors one at a time. A new address takes
the slot from its device, which takes the lowest free slot at the next boot if it is still on
the bus; that is also how a sensor gets its number before it is plugged in. A listed slot, a
listed sensor's address, an address that is not a thermometer ROM with a valid CRC, and a
device already in that slot change nothing; `check_assign(slot, address)` says which, first.
Like a forget, an assign that cannot write the table changes nothing.

`forget_and_save()` and `assign_and_save()` change the saved table without the reboot. The
sensors keep reading the devices they booted with until the next boot binds the saved table, so
several changes add up and one reboot applies them all; a change that puts the table back as
booted leaves nothing waiting. [`web_device_dashboard`](../web_device_dashboard/README.md) with
`dallas_scan_id:` lists the slots, forgets and assigns them over HTTP this way, numbered from 1;
its page does the same under **Settings → Temperature**.

## Offsets

Each slot can hold an offset, -5.0 to +5.0 °C in steps of 0.1, added to every reading of its
sensor before the `filters:`. It belongs to the slot number, like the `Temp N` name: an assign or
a swap leaves it where it is, a forget of one slot keeps it, and a device put into the slot
reads with it. A free slot can hold one for the sensor that takes it later. Listed slots take
none: their sensors have filters of their own.

`set_offset_and_save(slot, value)` saves the offset and applies it at once, with no reboot: the
slot's last reading is published again with it, so the change shows without waiting for the
next poll; with `filters:`, the filters decide when it shows. A value is rounded to the nearest
0.1, halves away from zero: 0.15 is 0.2, -0.25 is -0.3. Nothing changes when the value is out of
range, the slot is listed or past the table, the table cannot be written or its file did not
load this boot, or the write fails; `check_offset(slot, value)` and `can_set_offset()` say which,
first.

## Labels

With `storage: file` each slot can hold a label, text of the owner's own ("Boiler return",
«Подача») for a display menu and [`web_device_dashboard`](../web_device_dashboard/README.md) to
show in place of `Temp N`. The sensor keeps its name, so Home Assistant, the API, `web_server`,
Modbus, automations and thermostats still know it as `Temp N`. The label belongs to the slot number, as the offset does: an assign or a swap leaves
it where it is, a forget of one slot keeps it, a free slot holds one for the sensor that takes it
later, and Forget All and a factory reset clear every label. Listed slots take none: their YAML
names them, and a label stored for one is dropped at boot. With `storage: nvs` there are no
labels.

A label follows [`panel_text`](../panel_text/README.md#labels)'s rules, the same as a relay's:
well-formed UTF-8 with no control character, at most 24 characters (code points) once the spaces
at both ends are trimmed; empty means none. `set_label_and_save(slot, text)` saves it and it
shows at once: nothing is published and nothing waits for a reboot. Nothing changes when the text
breaks the rules, the slot is listed or past the table, the storage is not a file, the table
cannot be written or its file did not load this boot, or the write fails; `check_label(slot,
text)` and `can_set_label()` say which, first.

## From lambdas

Slots are 0-based here.

What this boot runs, fixed until the reboot:

- `max_sensors()`
- `pinned(slot)`: taken by `sensors:`, so it has no forget entry in a menu
- `sensors()`: the bound slots' sensors, in slot order
- `used_slots()`: slots up to the last bound one, free slots between them included, so a
  freed slot keeps its row
- `slot_name(slot)`: the sensor's name, `<prefix> N` for an empty slot
- `sensor(slot)`, `temperature(slot)`, `address(slot)`: `nullptr`, `NaN` and `0` when the slot
  is empty; the address is the ROM the sensor reads, `0` for a listed sensor that is not a
  1-Wire device

The saved table, the one the next boot binds:

- `saved_address(slot)`: the ROM it holds in the slot, `0` when empty
- `saved_slots()`: slots up to the last one holding a device
- `slot_pending(slot)`: the slot differs from boot
- `reboot_required()`: some slot does; safe to call from any task
- `can_forget(slot)`: the slot, or any slot for `-1`, holds a device and is not listed; for
  `-1`, an offset or a label on a slot that is not listed counts too
- `check_assign(slot, rom)`: what an assign would do, checked against it
- `valid_address(rom)`: a thermometer family and a valid CRC, the ROMs a slot can hold
- `can_save()`: the table can be written; false for a file whose partition did not mount

Changing it:

- `forget(slot)`, `-1` for every slot, and `assign(slot, rom)`: save and reboot
- `forget_and_save(slot)`, `assign_and_save(slot, rom)`: save without the reboot; false, with
  the table unchanged, when nothing would change or the table could not be written

Offsets, saved and applied at once:

- `MAX_OFFSET`, `OFFSET_STEP`: 5.0 and 0.1 °C
- `offset(slot)`: °C, `0` when the slot has none
- `check_offset(slot, value)`: what a set would do: `OK`, `BAD_SLOT`, `BAD_VALUE` (NaN, or past
  ±5.0 once rounded) or `LISTED_SLOT`
- `can_set_offset()`: `can_save()`, and the slot file loaded at boot
- `set_offset_and_save(slot, value)`: save, apply and publish the last reading again; true without
  a write when the slot has that offset already, false, with nothing changed, unless
  `check_offset()` is `OK`, `can_set_offset()` holds and the offset could be written

Labels, saved and shown at once; read them on the loop task, which changes them:

- `labels_supported()`: `storage: file`
- `label(slot)`: the label, `""` when the slot has none or is past the table
- `display_name(slot)`: what to show for the slot, its label or else `slot_name(slot)`
- `check_label(slot, text)`: what a set would do: `OK`, `BAD_SLOT`, `BAD_TEXT` (the rules above)
  or `LISTED_SLOT`
- `can_set_label()`: `labels_supported()` and `can_set_offset()`
- `set_label_and_save(slot, text)`: trim and save; `""` clears it. True without a write when the
  slot has that label already, false, with nothing changed, unless `check_label()` is `OK`,
  `can_set_label()` holds and the label could be written
