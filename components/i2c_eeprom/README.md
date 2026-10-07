# i2c_eeprom

Byte access to a 24Cxx-style I2C EEPROM or FRAM, for lambdas and for components such as
`jethome_board_info` and `fram_store`. Vendored from [pilotak/esphome-eeprom](https://github.com/pilotak/esphome-eeprom).

```yaml
i2c_eeprom:
  - id: eeprom_cpu
    size: 64KB
    address: 0x54
  - id: fram_cpu
    size: 64KB
    type: fram
    address: 0x52
```

## Options

| Option      | Default | Meaning |
| ----------- | ------- | ------- |
| `type`      | `eeprom` | `eeprom`, or `fram` for a ferroelectric part such as the FM24CL64B: it writes at bus speed, so a write is neither split at pages nor followed by a wait, and transfers both ways are cut every 128 bytes instead, which keeps each one far under the I2C driver's timeout. The bus stays busy for the whole call either way. `page_size` is refused with it |
| `size`      |         | The part's size in kilobits, as printed on it: `1KB`, `2KB`, `4KB`, `8KB`, `16KB`, `32KB`, `64KB`, `128KB`, `256KB` or `512KB`. Parts above `16KB` are addressed with two bytes; `4KB` to `16KB` parts are reached in their first 256 bytes only, as their block-select bits in the device address are not driven, and a request past that is refused |
| `page_size` | `8`     | EEPROM only. Bytes the part writes in one page. A write is split at these boundaries, since the chip rolls over to the start of the page instead of carrying into the next one. The default splits inside the real pages of every 24Cxx; naming the part's own page size only saves transactions. A power of two, at most the largest page the density has: `16` up to `16KB`, `32` for `32KB` and `64KB`, `64` for `128KB` and `256KB`, `128` for `512KB` |
| `address`   | `0x50`  | I2C address |
| `i2c_id`    |         | The bus; may be left out with a single bus |
| `on_setup`  |         | Automation run once the chip has answered at boot |

## From lambdas

- `get(addr, buffer, size)`: reads `size` bytes from memory address `addr`; false on a bus error
  or when the range does not fit the part
- `put(addr, buffer, size)`, `put(addr, byte)`: writes and waits out an EEPROM's write cycle;
  false on a bus error, an out-of-range write or a write-protected chip. A write crossing a page
  boundary (on FRAM, longer than 128 bytes) goes out as several transactions, so it is not
  all-or-nothing: a bus error part way through leaves the earlier ones written
- `is_write_protected()`, `set_write_protected(bool)`: while it is on, every write is refused and
  reads are untouched. `jethome_board_info` turns it on for the CPU board's EEPROM, which holds what
  the manufacturer put there; `protect_eeprom: false` there gives the writes back, and what becomes
  of that data is then your call
- `get_size()`: bytes
- `is_fram()`: the part was declared `type: fram`
- `is_connected()`: the chip answers a read

A chip that does not answer at boot marks the component failed; `get` and `put` keep reporting
the bus error.
