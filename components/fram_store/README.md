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

A store owns one range of the part: a 32-byte superblock at `base_offset`, then its regions.
The superblock records the layout the regions were placed by. **A firmware whose layout differs
in any way (a region added, removed, renamed or resized, or `size` or `base_offset` changed)
empties that store at its first boot**, and only that store: nothing outside its range is read
or written. A part that does not answer at boot, or whose superblock reads back differently
every time, is left alone: the store reports itself unavailable and does not format.

## One store per life cycle

Several stores can share a part, each in its own range; ranges that overlap are refused at build
time. Group regions into stores by how long their layout will hold, and keep to three rules:

- A store that has shipped never changes.
- New data goes into a new store in the part's spare range.
- A store that has to change moves to a new `base_offset` in the spare. Its old range stays
  reserved for as long as a firmware you could roll back to still uses it.

A rollback between builds that only add stores then wipes nothing: the older build leaves the
newer stores' ranges alone, and they are still there when the newer build boots again.

## Configuration

```yaml
i2c_eeprom:
  - id: fram_cpu
    size: 64KB
    type: fram
    address: 0x52

fram_store:
  - id: fram_store_totals
    eeprom_id: fram_cpu
    size: 8192
    base_offset: 0x200
    regions:
      - id: totals
        type: slot
        size: 64
  - id: fram_store_events
    eeprom_id: fram_cpu
    size: 8192
    base_offset: 0x300
    regions:
      - id: events
        type: ring
        record_size: 16
        capacity: 100
```

| Option | Default | Meaning |
| --- | --- | --- |
| `eeprom_id` | | The `i2c_eeprom` part, declared `type: fram` and holding `size` bytes. A `4KB` to `16KB` part is refused: `i2c_eeprom` reaches only its first 256 bytes. Without `eeprom_id` the store lives in RAM and is lost at reboot, which is what the host tests use; a device build says so in a warning |
| `size` | `8192` | The part's size in bytes |
| `base_offset` | `0` | Where the store starts; the bytes below it belong to someone else. A multiple of 4 |
| `regions` | | Slots (`size`: payload bytes, 1 to 4096) and rings (`record_size`: 1 to 1024 bytes, `capacity`: 1 to 4096 records), placed in this order after the superblock |

A slot takes `2 × (8 + size)` bytes, a ring `(6 + record_size) × capacity`, each rounded up to a
multiple of 4; the store's range ends after its last region. A layout that does not fit the part
is refused at build time.

## From C++

```cpp
struct Totals {
  uint16_t version;
  uint8_t reserved[6];
  int64_t wh[7];
} __attribute__((packed));
static_assert(sizeof(Totals) == 64, "the totals slot holds 64 bytes");

struct Event {
  uint32_t time;
  uint16_t code;
  uint8_t data[10];
} __attribute__((packed));
static_assert(sizeof(Event) == 16, "an events record is 16 bytes");

Totals current{};
if (id(totals).load(current)) { /* restored */ }
id(totals).save(current);       // false when the part did not take it

Event event{};
id(events).append(event);
id(events).peek(0, event);      // 0 is the newest
```

`load`, `save`, `append` and `peek` take a trivially copyable type and fail on one whose size is
not the region's. `load` checks the copy again on every call and falls back to the copy before
it when the current one is damaged. A region the store could not read at boot refuses every
write rather than write over contents it could not identify.

**When `load` fails on a slot that holds data, `save` refuses until a `load` succeeds or
`clear()` empties the slot.** A consumer that starts over from zero after a failed load would
otherwise overwrite both copies of the value it could not read; calling `clear()` is how it says
it means to.

`clear()` cut short by a power loss leaves a slot with its current value and a ring with its
newest records. `is_ready()` on the store, `has_data()` and `load_failed()` on a slot and
`count()` on a ring say what there is.

On the FM24CL64B a 64-byte save holds the shared I2C bus for about 2.5 ms, and emptying a whole
8 KiB store takes under half a second.
