# jethome_board_info

The identity a JetHome board carries in its own EEPROM: board name and version, the device
model, hardware revision and serial number, and the factory signature. The device reads and
reports. The CPU board keeps its EEPROM at `0x54`; a peripheral board, such as a meter's
metering board, can carry another, and each gets an instance of its own.

```yaml
i2c_eeprom:
  - id: eeprom_cpu
    size: 64KB
    address: 0x54

jethome_board_info:
  - id: board_info
    eeprom_id: eeprom_cpu
```

Declare it as a list in a package: packages merge a mapping into the mapping of the same key,
so a second instance declared that way would overwrite the first instead of joining it.

## Options

| Option           | Default | Meaning |
| ---------------- | ------- | ------- |
| `eeprom_id`      |         | The `i2c_eeprom` holding the identity |
| `protect_eeprom` | `true`  | Refuse every write to that EEPROM |
| `required`       | `true`  | Whether the EEPROM must hold a usable header. With `false`, a part that is blank, holds no JEEFS, is damaged or does not answer is logged as a warning and the component keeps running; with `true` it is an error and the component is marked failed. A part that does not answer at all still fails its `i2c_eeprom` either way |

## What it reads

The EEPROM starts with a 256-byte board header (JEEFS header v3 or v4). When the header's
filesystem version is 1, on v3 as on v4, a file chain follows it, in which `device.id`
describes the device built on the board. Everything is read once at boot, after the EEPROM is
up, CRC-checked and listed by `dump_config`. Nothing is written, and by default nothing else
writes either: the EEPROM holds what the manufacturer put there, so `protect_eeprom` has
`i2c_eeprom` refuse every `put` on it; turning it off gives the writes back, and what becomes of
that data is then your call. A header that fails its checks leaves every field empty; a
`device.id` that fails its checks is treated as absent. The USID carries the last ten digits of
the device serial, so a `device.id` serial is cross-checked against it.

## From lambdas

- `is_valid()`: the header parsed; every getter below is empty otherwise
- `get_header_status()`: why it did not, as a read status (below); `OK` once it did
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
  in the chain, CRC-checked; both return a read status

| Read status   | Meaning |
| ------------- | ------- |
| `OK`          | Found, and its data matches the CRC the chain records |
| `NO_EEPROM`   | The EEPROM did not answer, or was not read yet |
| `NO_HEADER`   | No board header: the part is blank, erased, or holds something other than JEEFS |
| `UNSUPPORTED` | A header version or filesystem version this firmware does not read |
| `CORRUPT`     | A header, file header or file whose CRC does not match, or a broken chain |
| `NOT_FOUND`   | No file of that name, or no file area at all |
| `TOO_LARGE`   | The file is bigger than the buffer |
| `BAD_NAME`    | Not a name a JEEFS file can have |
| `READ_ERROR`  | The EEPROM stopped answering part way through |
