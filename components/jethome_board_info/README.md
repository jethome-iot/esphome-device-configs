# jethome_board_info

The identity a JetHome board carries in the EEPROM of its CPU board: board name and version,
the device model, hardware revision and serial number, and the factory signature that binds
the board to its chip (it covers the CPU id, MAC and USID, not the other fields). The device
reads and reports; it does not verify.

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
and leaves every field empty; a `device.id` that fails its checks is treated as absent.

## Verifying the signature

The signature is secp256r1 (ECDSA over SHA-256) of `cpuid:MAC:usid`: the chip's factory eFuse
MAC as `AA:BB:CC:DD:EE:FF`, its custom eFuse MAC as `AABBCCDDEEFF`, and the header's USID.
It is genuine only when those match the chip it is read from and the signature verifies
against JetHome's public key; the component publishes the header's `cpuid`, `mac` and `usid`
and the signature bytes, the key and the chip's eFuses are for the verifier. The USID carries
the last ten digits of the serial, so the record's serial is cross-checked against it here;
the model and hardware revision are covered by the record's CRC only.

## From lambdas

- `is_valid()`: the header parsed; every getter below is empty otherwise
- `has_device_identity()`: a `device.id` record was found and parsed
- `get_boardname()`, `get_boardversion()`: from the header
- `get_device_serial()`: from `device.id`, or the header's serial on v3
- `get_device_model()`, `get_hw_revision()`: from `device.id`, empty without one
- `get_board_serial()`: v4 headers only
- `get_usid()`, `get_cpuid()`, `get_mac_str()`, `get_mac()`, `get_timestamp()`,
  `get_header_version()`
- `get_signature_version()`, `get_signature()`: the algorithm (`0` none, `2` secp256r1) and
  the 64 bytes `r || s` of the signature that covers the device: the record's when there is
  one, else the header's
- `has_serial_check()`, `serial_matches_usid()`: whether the record's serial agrees with the
  signed USID
- `find_file(name, offset, size, crc32)`, `read_file(name, buffer, size, &read)`: other files
  in the chain, CRC-checked
