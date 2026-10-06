# OneWire Temperature Sensors

DS18B20 sensors on the 1-Wire connector (DS2484 bridge at `0x18`,
`devices/JXD/packages/boards/jxd-d6-r6-rev1.2.yaml`) show up as `Temp 1`, `Temp 2`, … — one sensor per
device found at boot, up to sixteen. Each is a sensor in Home Assistant, a row in the
**Temperatures** menu and on the status page, and a holding register `0x0000`-`0x000F`.

## Slots

At boot every new sensor takes the lowest free slot, in bus order, and the slot keeps its
ROM address from then on; adding, removing or swapping other sensors does not move it.
Empty slots have no sensor, so a device plugged in later appears after the next reboot. An
unplugged sensor reads `--` (`0x8000` over Modbus); the log notes it once when it
stops answering and once when it is back, no reboot needed. A reading of exactly 85.0 °C,
the DS18B20 power-on value, is dropped.

Only Dallas temperature sensors take a slot — DS18B20, DS18S20, DS1822, DS1825 and
DS28EA00. Any other 1-Wire device on the bus is skipped and logged as `Not a temperature
sensor`.

To choose the order, connect the sensors one at a time, rebooting after each, or move them
afterwards from the web dashboard (below).

## Addresses

**Temperatures → Temp N** on the panel and **Settings → Temperature** on the web dashboard show
each slot's ROM address, e.g. `0xeb01227905460228`. The boot log lists them too
(`ds2484: Found devices`).

## Backup and restore

The slots and their offsets are kept in `/littlefs/config/dallas_scan_temps.json` on the user
storage partition, next to the relay and input settings and the automations, so a backup of the
partition carries them and a restore puts every sensor back in its slot, with its offset, on this
controller or on a replacement: restore, then reboot. A rule that reads `Temp 3` reads the same
sensor as before.

The file can also be edited by hand, then the device rebooted; the format is in
[components/dallas_scan](../components/dallas_scan/README.md#storage).

## Your own sensors

To fix a slot, or to have a sensor with your own name, id and filters, declare it in YAML and
list it in `sensors:` in `devices/JXD/packages/features/temperature.yaml`: its position in the
list is its slot, so its menu row, status page row and Modbus register. Listed sensors take
the first slots, in that order; the scan fills the slots after them.

```yaml
sensor:
  - platform: dallas_temp
    id: boiler
    name: "Boiler"
    address: 0x8a0122791699dd28
    filters:
      - filter_out: 85.0

dallas_scan:
  sensors: [boiler]
```

A 1-Wire sensor in the list needs an `address:`; its device keeps that slot. The menu entry of
a listed sensor shows the address but has no forget and no offset.

## Forgetting

**Temperatures → Temp N → Confirm** clears the slot and reboots; the sensor in it, or a new
one, takes the lowest free slot again. The other slots keep their numbers, and a freed slot
before them keeps its `Temp N` row reading `--`; its menu entry says `Free slot`.
**Settings → Temp sensors → Confirm** clears every slot but the listed ones, and every
[offset](#offset), so sensors are numbered again in bus order and no offset lands on another
sensor. Factory reset clears them too.

The web dashboard does the same without the panel, which is the only way on a device without
a display: **Settings → Temperature** lists the slots with their readings and ROM addresses,
and **Forget** on a slot or **Forget All**, clicked twice, clears them. The dashboard does not
reboot: the sensors keep reading as before, though Forget All clears the offsets at once, and a
banner offers **Reboot now** until the restart, or until every change is undone. Several changes
add up and one reboot applies them all. The panel's Confirm on a slot the dashboard changed
reboots into what it saved. Scripts can use its routes, in
[components/web_device_dashboard](../components/web_device_dashboard/README.md).

## Moving and assigning

Only the web dashboard does this: **Edit** on a slot in **Settings → Temperature** takes a slot
number and a ROM address, and **Assign by Address** opens the same dialog on the lowest free slot
with an empty address. The dialog says what will change before it saves; the device takes that
slot at the next reboot, as for a forget on the dashboard.

- The address of a sensor in another slot moves it there; if a sensor holds that slot, the two
  swap.
- Another address puts that device into the slot — for a sensor that is not plugged in yet, its
  number is then waiting for it. The sensor that held the slot loses it and, if still
  connected, takes the lowest free slot at the next boot.

A `Temp N` name and its Modbus register belong to the slot: a sensor moved to slot 3 reads as
`Temp 3`, on that slot's register. Listed slots cannot be assigned; the tab shows them as
`Fixed in YAML`.

## Offset

Every DS18B20 reads off by an amount of its own: a genuine part is usually 0.1-0.3 °C low, a
clone can be several degrees off, and heat from a board or an enclosure adds more. Each slot can
hold an offset, from -5.0 to +5.0 °C in steps of 0.1, added to its sensor's reading.

- On the panel: **Temperatures → Temp N → Offset**. CENTER opens the row, LEFT and RIGHT step by
  0.1, CENTER or BACK saves it.
- On the web dashboard: the slot's **Edit** dialog in **Settings → Temperature**, which also
  sets one on a free slot, for the sensor that takes it later.

It applies at once, with no reboot: Home Assistant, Modbus, automations and thermostats all get
the corrected reading. It belongs to the slot number, like the `Temp N` name: it stays through
an assign, a swap or a forget of that slot, and a sensor that takes the slot reads with it.
Forget All and a factory reset clear every offset. Listed sensors take none: they have their own
`filters:` (above). The details are in
[components/dallas_scan](../components/dallas_scan/README.md#offsets).

## More slots

Raise `max_sensors` in `devices/JXD/packages/features/temperature.yaml` and add the registers in
`devices/JXD/packages/features/modbus-server.yaml`, the README line and `TEMP_COUNT` in
`scripts/modbus_probe.py`. The slots already taken keep their sensors.

## The component

Options, behavior and the lambda API: [components/dallas_scan/README.md](../components/dallas_scan/README.md).
