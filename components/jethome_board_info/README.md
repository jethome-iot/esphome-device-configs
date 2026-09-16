# jethome_board_info

The identity a JetHome board carries in the EEPROM of its CPU board: board name and version,
the device model, hardware revision and serial number, and the factory signature. The device
reads and reports.

```yaml
i2c_eeprom:
  - id: eeprom_cpu
    size: 64KB
    address: 0x54

jethome_board_info:
  id: board_info
  eeprom_id: eeprom_cpu
```

## Options

| Option      | Meaning |
| ----------- | ------- |
| `eeprom_id` | The `i2c_eeprom` holding the identity |

## What it reads

The EEPROM starts with a 256-byte board header (JEEFS header v3 or v4) and, on v4 boards,
continues with a file chain in which `device.id` describes the device built on the board.
Everything is read once at boot, after the EEPROM is up, CRC-checked and listed by
`dump_config`. Nothing is written. A header that fails its checks marks the component failed
and leaves every field empty; a `device.id` that fails its checks is treated as absent. The
USID carries the last ten digits of the device serial, so a `device.id` serial is
cross-checked against it.

## From lambdas

- `is_valid()`: the header parsed; every getter below is empty otherwise
- `has_device_identity()`: a `device.id` record was found and parsed
- `get_boardname()`, `get_boardversion()`: from the header
- `get_device_serial()`: from `device.id`, or the header's serial on v3
- `get_device_model()`, `get_hw_revision()`: from `device.id`, empty without one
- `get_board_serial()`: v4 headers only
- `get_usid()`, `get_cpuid()`, `get_mac_str()`, `get_mac()`, `get_timestamp()`,
  `get_header_version()`
- `get_signature_version()`, `get_signature()`: the version byte (`0` never signed) and the 64
  signature bytes that cover the device: the record's when there is one, else the header's
- `has_serial_check()`, `serial_matches_usid()`: whether the record's serial agrees with the
  header's USID
- `find_file(name, offset, size, crc32)`, `read_file(name, buffer, size, &read)`: other files
  in the chain, CRC-checked
