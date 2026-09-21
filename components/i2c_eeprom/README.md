# i2c_eeprom

Byte access to a 24Cxx-style I2C EEPROM, for lambdas and for components such as
`jethome_board_info`. Vendored from [pilotak/esphome-eeprom](https://github.com/pilotak/esphome-eeprom).

```yaml
i2c_eeprom:
  - id: eeprom_cpu
    size: 64KB
    address: 0x54
```

## Options

| Option      | Default | Meaning |
| ----------- | ------- | ------- |
| `size`      |         | The part's size in kilobits, as printed on it: `1KB`, `2KB`, `4KB`, `8KB`, `16KB`, `32KB`, `64KB`, `128KB`, `256KB` or `512KB`. Parts above `16KB` are addressed with two bytes; `4KB` to `16KB` parts are reached in their first 256 bytes only, as their block-select bits in the device address are not driven, and a request past that is refused |
| `page_size` | `8`     | Bytes the part writes in one page. A write is split at these boundaries, since the chip rolls over to the start of the page instead of carrying into the next one. The default splits inside the real pages of every 24Cxx; naming the part's own page size only saves transactions. A power of two, at most the largest page the density has: `16` up to `16KB`, `32` for `32KB` and `64KB`, `64` for `128KB` and `256KB`, `128` for `512KB` |
| `address`   | `0x50`  | I2C address |
| `i2c_id`    |         | The bus; may be left out with a single bus |
| `on_setup`  |         | Automation run once the chip has answered at boot |

## From lambdas

- `get(addr, buffer, size)`: reads `size` bytes from memory address `addr`; false on a bus error
  or when the range does not fit the part
- `put(addr, buffer, size)`, `put(addr, byte)`: writes and waits out the write cycle; false on a
  bus error, an out-of-range write or a write-protected chip. A write crossing a page boundary
  goes out as one transaction per page, so it is not all-or-nothing: a bus error part way through
  leaves the earlier pages written
- `is_write_protected()`, `set_write_protected(bool)`: while it is on, every write is refused and
  reads are untouched. `jethome_board_info` turns it on for the CPU board's EEPROM, which holds what
  the manufacturer put there; `protect_eeprom: false` there gives the writes back, and what becomes
  of that data is then your call
- `get_size()`: bytes
- `is_connected()`: the chip answers a read

A chip that does not answer at boot marks the component failed; `get` and `put` keep reporting
the bus error.
