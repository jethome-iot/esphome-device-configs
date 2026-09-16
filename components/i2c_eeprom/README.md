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

| Option     | Default | Meaning |
| ---------- | ------- | ------- |
| `size`     |         | The part's size in kilobits, as printed on it: `1KB`, `2KB`, `4KB`, `8KB`, `16KB`, `32KB`, `64KB`, `128KB`, `256KB` or `512KB`. Parts above `16KB` are addressed with two bytes; `4KB` to `16KB` parts are reached in their first 256 bytes only, as their block-select bits in the device address are not driven, and a request past that is refused |
| `address`  | `0x50`  | I2C address |
| `i2c_id`   |         | The bus; may be left out with a single bus |
| `on_setup` |         | Automation run once the chip has answered at boot |

## From lambdas

- `get(addr, buffer, size)`: reads `size` bytes from memory address `addr`; false on a bus error
  or when the range does not fit the part
- `put(addr, buffer, size)`, `put(addr, byte)`: writes and waits out the write cycle; false on a
  bus error or an out-of-range write. A write is not split at page boundaries
- `get_size()`: bytes
- `is_connected()`: the chip answers a read

A chip that does not answer at boot marks the component failed; `get` and `put` keep reporting
the bus error.
