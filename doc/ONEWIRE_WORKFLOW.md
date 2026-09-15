# OneWire Temperature Sensors

Eight slots, `Temp1` to `Temp8`, for DS18B20 sensors on the 1-Wire connector (DS2484
bridge at `0x18`, `devices/JXD/packages/boards/jxd-d6-r6-rev1.2.yaml`). Each slot is a sensor in Home
Assistant, a row in the **Temperatures** menu and on the status page, and holding register
`0x0000`-`0x0007`.

## Slots

At boot every new sensor takes the lowest free slot, in bus order, and the slot keeps its
ROM address in flash from then on; adding, removing or swapping other sensors does not
move it. An unplugged sensor reads `--` (`0x8000` over Modbus). Empty slots log
`Index 8 out of range` at boot; harmless. A reading of exactly 85.0 °C, the DS18B20
power-on value, is dropped.

Only Dallas temperature sensors take a slot — DS18B20, DS18S20, DS1822, DS1825 and
DS28EA00. Any other 1-Wire device on the bus is skipped and logged as `Not a temperature
sensor`.

To choose the order, connect the sensors one at a time, rebooting after each.

## Addresses

**Settings → Temp sensors → TempN** shows the slot's ROM address, e.g.
`0xeb01227905460228`. The boot log lists them too (`ds2484: Found devices`).

## Pinning

Replace `index:` with the address in `devices/JXD/packages/features/temperature.yaml`:

```yaml
  - platform: dallas_temp
    name: "Temp2"
    id: temp_2
    address: 0xeb01227905460228
```

A pinned slot always holds that sensor; forgetting it has no effect.

## Forgetting

**Settings → Temp sensors → TempN → Confirm** clears the slot and reboots; the sensor in
it, or a new one, takes the lowest free slot again. **All** clears every slot, so sensors
are numbered again in bus order. Factory reset clears them too.

## More slots

Add a `dallas_temp` sensor with the next `index`, grow `dallas_slots`, and add it to
`temperatures` and to `sensors` in the slot lambda (`devices/JXD/packages/features/temperature.yaml`);
add a **Temperatures** label and a **Temp sensors** entry (`devices/JXD/packages/display/menu.yaml`), a
register (`devices/JXD/packages/features/modbus-server.yaml`), the README line and `TEMP_COUNT` in
`scripts/modbus_probe.py`. Growing the table empties it once.
