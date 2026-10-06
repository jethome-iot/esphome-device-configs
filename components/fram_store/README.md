# fram_store

Crash-safe storage for state that has to be current when the power fails, on a FRAM part such
as the CPU board's FM24CL64B (8 KiB at I2C `0x52`). FRAM takes a write at bus speed and some
10¹⁴ of them, so a value can be saved on every change instead of on a timer.

## What it gives you

- **`slot`**: one value of a fixed size, kept in two copies. A save goes to the older copy and
  becomes the current one only once it is complete, so a save cut short by a power loss loses
  that update, never the value before it.
- **`ring`**: an append-only log of fixed-size records; the oldest is dropped once it is full. At
  boot the newest intact record is taken as the head, past up to four damaged ones such as an
  append cut short; a ring with more is emptied, so later appends stay readable.

The store opens with a 32-byte superblock at `base_offset` that records the layout the regions
were placed by. **A firmware whose layout differs in any way (a region added, removed, renamed or
resized, or `size` or `base_offset` changed) empties the whole store at its first boot.** Keep
the layout of a shipped device as it is, and version what a slot holds inside its payload.

Nothing below `base_offset` is read or written. A part that does not answer at boot is left
alone: the store reports itself unavailable and does not format.

## Configuration

```yaml
i2c_eeprom:
  - id: fram_cpu
    size: 64KB
    type: fram
    address: 0x52

fram_store:
  - id: fram_store_cpu
    eeprom_id: fram_cpu
    size: 8192
    base_offset: 0x200
    regions:
      - id: totals
        type: slot
        size: 64
      - id: events
        type: ring
        record_size: 16
        capacity: 100
```

| Option | Default | Meaning |
| --- | --- | --- |
| `eeprom_id` | | The `i2c_eeprom` part, declared `type: fram` and holding `size` bytes. Without it the store lives in RAM and is lost at reboot, which is what the host tests use |
| `size` | `8192` | The part's size in bytes |
| `base_offset` | `0` | Where the store starts; the bytes below it belong to someone else. A multiple of 4 |
| `regions` | | Slots (`size`: payload bytes, 1 to 4096) and rings (`record_size`: 1 to 1024 bytes, `capacity`: 1 to 4096 records), placed in this order after the superblock |

A slot takes `2 × (8 + size)` bytes, a ring `(6 + record_size) × capacity`, each rounded up to a
multiple of 4. A layout that does not fit is refused at build time.

## From C++

```cpp
struct Totals {
  uint16_t version;
  int64_t wh[3];
} __attribute__((packed));

Totals totals{};
if (id(totals_slot).load(totals)) { /* restored */ }
id(totals_slot).save(totals);   // false when the part did not take it

id(events).append(record);      // fixed-size record
id(events).peek(0, record);     // 0 is the newest
```

`load`, `save`, `append` and `peek` take a trivially copyable type and fail on one whose size is
not the region's. `load` checks the copy again on every call. A region the store could not read
at boot refuses every write rather than write over contents it could not identify.
`is_ready()` on the store, `has_data()` on a slot and `count()` on a ring say what there is.

On the FM24CL64B a 64-byte save holds the shared I2C bus for about 2.5 ms, and emptying the
store, at its first boot or after a layout change, takes under half a second.
