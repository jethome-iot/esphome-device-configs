# OneWire Temperature Sensors

DS18B20 sensors on the 1-Wire connector (DS2484 bridge at `0x18`,
`devices/JXD/packages/boards/jxd-d6-r6-rev1.2.yaml`) show up as `Temp1`, `Temp2`, … — one sensor per
device found at boot, up to sixteen. Each is a sensor in Home Assistant, a row in the
**Temperatures** menu and on the status page, and a holding register `0x0000`-`0x000F`.

## Slots

At boot every new sensor takes the lowest free slot, in bus order, and the slot keeps its
ROM address in flash from then on; adding, removing or swapping other sensors does not
move it. Empty slots have no sensor, so a device plugged in later appears after the next
reboot. An unplugged sensor reads `--` (`0x8000` over Modbus). A reading of exactly
85.0 °C, the DS18B20 power-on value, is dropped.

Only Dallas temperature sensors take a slot — DS18B20, DS18S20, DS1822, DS1825 and
DS28EA00. Any other 1-Wire device on the bus is skipped and logged as `Not a temperature
sensor`.

To choose the order, connect the sensors one at a time, rebooting after each.

## Addresses

**Settings → Temp sensors → TempN** shows the slot's ROM address, e.g.
`0xeb01227905460228`. The boot log lists them too (`ds2484: Found devices`).

## Pinning

Add the slot to `addresses:` in `devices/JXD/packages/features/temperature.yaml`:

```yaml
dallas_scan:
  addresses:
    2: 0xeb01227905460228
```

A pinned slot always holds that sensor; forgetting it has no effect.

## Forgetting

**Settings → Temp sensors → TempN → Confirm** clears the slot and reboots; the sensor in
it, or a new one, takes the lowest free slot again. **All** clears every slot, so sensors
are numbered again in bus order. Factory reset clears them too.

## More slots

Raise `max_sensors` in `devices/JXD/packages/features/temperature.yaml` and add the registers in
`devices/JXD/packages/features/modbus-server.yaml`, the README line and `TEMP_COUNT` in
`scripts/modbus_probe.py`. Changing `max_sensors` empties the table once.

## The `dallas_scan` component

`components/dallas_scan` does the scanning; the sensors are not in the YAML.

| Option            | Default | Meaning                                                        |
| ----------------- | ------- | -------------------------------------------------------------- |
| `one_wire_id`     |         | The bus to scan                                                |
| `max_sensors`     | `8`     | Slots, and the size of the table in flash                      |
| `name_prefix`     | `Temp`  | Sensor names are the prefix and the slot number                |
| `resolution`      | `12`    | Bits, 9-12, written to the sensors at boot                     |
| `addresses`       |         | Slot number → ROM address, pins the slot                       |
| `update_interval` | `60s`   | One conversion for the whole bus, then one read per loop pass  |
| `web_server`      |         | `sorting_group_id` and `sorting_weight`; slot N gets weight + N - 1 |

From lambdas: `id(temps)->sensors()` (bound slots in order), `sensor(slot)`,
`temperature(slot)`, `address(slot)`, `max_sensors()`, `forget(slot)` (`-1` = all, reboots).
