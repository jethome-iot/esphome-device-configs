# OneWire Temperature Sensors

DS18B20 sensors on the 1-Wire connector (DS2484 bridge at `0x18`,
`devices/JXD/packages/boards/jxd-d6-r6-rev1.2.yaml`) show up as `Temp 1`, `Temp 2`, … — one sensor per
device found at boot, up to sixteen. Each is a sensor in Home Assistant, a row in the
**Temperatures** menu and on the status page, and a holding register `0x0000`-`0x000F`.

## Slots

At boot every new sensor takes the lowest free slot, in bus order, and the slot keeps its
ROM address in flash from then on; adding, removing or swapping other sensors does not
move it. Empty slots have no sensor, so a device plugged in later appears after the next
reboot. An unplugged sensor reads `--` (`0x8000` over Modbus); the log notes it once when it
stops answering and once when it is back, no reboot needed. A reading of exactly 85.0 °C,
the DS18B20 power-on value, is dropped.

Only Dallas temperature sensors take a slot — DS18B20, DS18S20, DS1822, DS1825 and
DS28EA00. Any other 1-Wire device on the bus is skipped and logged as `Not a temperature
sensor`.

To choose the order, connect the sensors one at a time, rebooting after each.

## Addresses

**Temperatures → Temp N** shows the slot's ROM address, e.g. `0xeb01227905460228`. The boot
log lists them too (`ds2484: Found devices`).

## Pinning

Add the slot to `slots:` in `devices/JXD/packages/features/temperature.yaml`:

```yaml
dallas_scan:
  slots:
    2: 0xeb01227905460228
```

A pinned slot always holds that sensor; forgetting it has no effect.

## Your own sensor in a slot

A slot can be served by a sensor declared in YAML, with its own name, id and filters; it then
shows up in the menu, on the status page and over Modbus like the others:

```yaml
sensor:
  - platform: dallas_temp
    id: boiler
    name: "Boiler"
    address: 0x8a0122791699dd28
    filters:
      - filter_out: 85.0

dallas_scan:
  slots:
    4:
      address: 0x8a0122791699dd28
      sensor: boiler
```

## Forgetting

**Temperatures → Temp N → Confirm** clears the slot and reboots; the sensor in it, or a new
one, takes the lowest free slot again. **Settings → Temp sensors → Confirm** clears every
slot, so sensors are numbered again in bus order. Factory reset clears them too.

## More slots

Raise `max_sensors` in `devices/JXD/packages/features/temperature.yaml` and add the registers in
`devices/JXD/packages/features/modbus-server.yaml`, the README line and `TEMP_COUNT` in
`scripts/modbus_probe.py`. Changing `max_sensors` empties the table once.

## The component

Options, behavior and the lambda API: [components/dallas_scan/README.md](../components/dallas_scan/README.md).
