# meter_calibration

The factory calibration of a meter's metering board: the file `meter.cal` that the factory
writes into the board's own EEPROM, read once at boot and checked against the board the
firmware was built for. The component reports what it found; nothing here writes the EEPROM or
touches a metering chip.

```yaml
i2c_eeprom:
  - id: eeprom_meter
    size: 64KB
    address: 0x56

jethome_board_info:
  - id: meter_board_info
    eeprom_id: eeprom_meter
    # Boards shipped before the factory wrote records carry a blank part.
    required: false

meter_calibration:
  board_info_id: meter_board_info
  chip: atm90e32
  board_model: JXD-D3-PM3-3
```

## Options

| Option          | Default | Meaning |
| --------------- | ------- | ------- |
| `board_info_id` |         | The `jethome_board_info` over the metering board's EEPROM |
| `chip`          |         | `atm90e32` (3 channels) or `bl0906` (6 channels): the chip the firmware drives |
| `board_model`   |         | The metering board the firmware is built for, as the record names it, e.g. `JXD-D3-PM3-3` |
| `pga_code`      | per chip | The PGA setting the firmware runs the chip at: `0x15` (2X on every channel) for `atm90e32`, `0` (x1) for `bl0906`. A record taken at another setting is not used |

It sets up after the board info and before any metering driver.

## Statuses

None of them stops metering; they decide which coefficients a driver may take.

| Status        | When | Log |
| ------------- | ---- | --- |
| `factory`     | The record is sound, belongs to this board, and the line check passed (flag bit 5) | info |
| `unverified`  | The same, but bit 5 is clear: the station never finished. The record is still usable | warning |
| `missing`     | The EEPROM has no board header (blank, erased) or no `meter.cal` | info |
| `corrupt`     | A CRC fails (board header, file chain, file, record), the record is not 256 bytes, or a field holds a value the format does not allow | warning |
| `foreign`     | The chip, channel count, board model or board serial differs from this board's, the board header has no serial, or the chip type is unknown | warning |
| `unsupported` | An unknown record, signature, header or filesystem version; flag bits or reserved bytes version 1 leaves zero; a CT nominal ratio other than 2000; a PGA setting other than the firmware's; a file longer than 256 bytes | warning |
| `no_eeprom`   | The metering board's EEPROM does not answer; its `i2c_eeprom` is marked failed | warning |

`dump_config` prints the status, why, and with a usable record its board, date, station and
coefficients.

## From lambdas

- `get_status()`, `get_status_str()`: the status, and its name from the table above
- `get_reason()`: why it is not `factory`, in words; empty when it is
- `has_record()`: `factory` or `unverified`, so the record is this board's and sound;
  `is_verified()`: `factory` only
- `get_record()`: the whole record, all zeroes without one
- `get_atm90e32()`, `get_bl0906()`: the chip's part of the record, `nullptr` without a record
  or for the other chip
- `get_board_model()`, `get_board_revision()`, `get_board_serial()`, `get_station_id()`,
  `get_ref_instrument_id()`, `get_timestamp()`, `get_flags()`
- `get_record_crc32()`: the record's own CRC, which names this factory record
- `get_read_status()`, `get_record_check()`, `get_match_check()`: which step failed

## The EEPROM image

The metering board carries a CAT24C64 (8192 bytes) at I2C address `0x56`. The factory writes it
once per board; the firmware only reads it.

| Address | Size | Content |
| ------- | ---- | ------- |
| 0       | 256  | JEEFS board header, version 4 (board-scoped) |
| 256     | 28   | JEEFS file header of `meter.cal` |
| 284     | 256  | `meter.cal`, the record below |
| 540     | —    | Free; further files of the board may follow in the chain |
| 8160    | 32   | The test station's scratch page, outside the file chain |

The board header follows JEEFS header v4: magic `JETHOME\0`, `version` 4, `fs_version` 1,
`boardname` the board model (`JXD-D3-PM3-3`, `JXD-D3-PM1-6`), `boardversion` its revision
(`1.2`, `1.1.0`), `board_serial` the metering board's serial number, never empty, `usid`, `cpuid` and `mac`
left empty, `timestamp` the time the header was written, and the CRC-32 over bytes 0–251. The
signature fields are the factory's; the firmware does not read them. The file header follows JEEFS filesystem v1: name `meter.cal`,
`data_size` 256, the CRC-32 of the 256 data bytes, `next` 0 when `meter.cal` is the last file,
and the CRC-32 over the file header's bytes 0–23. A `device.id` is not needed on this part.

## The record, `meter.cal`

256 bytes, little-endian, packed. Text fields are ASCII padded with NUL; a text that fills its
field has no NUL. Every byte the record does not use is zero.

| Offset | Size | Field | Encoding |
| ------ | ---- | ----- | -------- |
| 0   | 8   | `magic` | `JHMCAL\0\0` |
| 8   | 1   | `record_version` | `1` |
| 9   | 1   | `signature_version` | `0`; any other value is not used |
| 10  | 1   | `chip_type` | `1` ATM90E32AS, `2` BL0906 |
| 11  | 1   | `channel_count` | current channels: `3` for ATM90E32AS, `6` for BL0906 |
| 12  | 32  | `board_model` | text, e.g. `JXD-D3-PM3-3` |
| 44  | 8   | `board_revision` | text, e.g. `1.2` |
| 52  | 32  | `board_serial` | text; equal to `board_serial` in the board header, which is not empty |
| 84  | 4   | `ref_voltage_mv` | u32, the calibration point's voltage, mV |
| 88  | 4   | `ref_current_ua` | u32, the calibration point's secondary current, µA |
| 92  | 2   | `ct_nominal_ratio` | u16, `2000`: coefficients refer to the primary current of a 2000:1 CT |
| 94  | 1   | `line_freq_hz` | `50` or `60` |
| 95  | 1   | `pga_code` | ATM90E32: MMode1[5:0]; BL0906: `0` (x1 on every channel) |
| 96  | 2   | `ambient_ddeg` | i16, ambient temperature, 0.1 °C |
| 98  | 2   | `flags` | u16, below |
| 100 | 12  | `station_id` | text |
| 112 | 16  | `ref_instrument_id` | text, the reference meter's serial number |
| 128 | 112 | body | per chip, below |
| 240 | 8   | `timestamp` | i64, Unix time of the calibration, UTC, seconds |
| 248 | 4   | `operator_code` | u32 |
| 252 | 4   | `crc32` | CRC-32 (IEEE, as JEEFS) over bytes 0–251 |

`flags`: bit 0 voltage and current gains, bit 1 voltage and current offsets, bit 2 active and
reactive power offsets, bit 3 energy gain, bit 4 phase, each set when that group was
calibrated; bit 5 set once the check on the line passed. Bits 6–15 are zero.

### ATM90E32AS body (`chip_type` 1)

| Offset | Size | Field | Encoding |
| ------ | ---- | ----- | -------- |
| 128 + 16·p | 16 | phase p = 0, 1, 2 (A, B, C) | `ugain` u16, `igain` u16, `uoffset` i16, `ioffset` i16, `poffset` i16, `qoffset` i16, `pqgain` i16, `phi` u16 |
| 176 | 1  | `current_scale` | `1`, or `2` when the current gains were taken at half scale |
| 177 | 1  | reserved | `0` |
| 178 | 4  | `plconst` | u32, `0` for the firmware's default |
| 182 | 6  | `err_pf1[3]` | i16, active power error at the calibration point, PF 1, 0.01 % |
| 188 | 6  | `err_pf05l[3]` | i16, the same at PF 0.5 inductive, 0.01 % |
| 194 | 6  | `err_worst[3]` | i16, the worst error over the acceptance points, 0.01 % |
| 200 | 40 | reserved | `0` |

The phase fields are register codes as the chip takes them: `ugain` and `igain` unsigned with
32768 as unity; offsets and `pqgain` two's complement; `phi` bit 15 set to delay the voltage,
bits 7:0 the delay in 2.048 MHz cycles, bits 14:8 zero. With flag bit 0 set, no `ugain` or
`igain` is zero.

### BL0906 body (`chip_type` 2)

| Offset | Size | Field | Encoding |
| ------ | ---- | ----- | -------- |
| 128 | 4  | `kv_milli` | u32, LSB per volt × 1000 |
| 132 | 24 | `ki_milli[6]` | u32, LSB per primary ampere at the nominal ratio × 1000 |
| 156 | 24 | `kp_micro[6]` | u32, LSB per watt × 10⁶ |
| 180 | 12 | `phase_mdeg[6]` | i16, 0.001°; positive when the board reads P high at PF 0.5 inductive |
| 192 | 2  | reserved | `0` |
| 194 | 12 | `i_off_ma[6]` | u16, primary mA, subtracted in squares |
| 206 | 12 | `p_off_cw[6]` | i16, 0.01 W, subtracted from P |
| 218 | 12 | `err_worst[6]` | i16, the worst error over the acceptance points, 0.01 % |
| 230 | 10 | reserved | `0` |

With flag bit 0 set, neither `kv_milli` nor any `ki_milli` is zero; with bit 3 set, no
`kp_micro` is.
