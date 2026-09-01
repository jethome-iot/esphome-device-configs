# Decisions

Why things are the way they are. This file exists so that rationale has somewhere to live
other than a comment block in the source — see the comment budget in `CLAUDE.md`.

One entry per decision: what was decided, what it rules out, and what would make it worth
revisiting. Detail that only matters once lives in the commit message; link it by SHA.

---

## No ESPHome fork — the repo builds against the pinned upstream release

`requirements.txt` pins one upstream ESPHome version and nothing pulls a
`github://jethome-iot/esphome` source. Anyone can clone and build with `pip install -r
requirements.txt`, which is the point of a public configs repo.

The price is that anything the fork adds is unavailable here, and features that need core
changes have to be reworked to fit upstream or dropped. `modbus_server_group` is the worked
example: 2026.8.2 moved Modbus server support upstream, but its `modbus_server` component
serves read-coils and read-discrete-inputs from one shared bit table and rejects duplicate
addresses — and our register map deliberately puts relays and inputs at the same address.
Deriving from `modbus::ModbusServerDevice` and overriding `on_read_coils()` /
`on_read_discrete_inputs()` separately keeps the two address spaces apart without a fork
(`720bee8`).

Revisit if upstream ever gains a way to separate the two bit tables.

## `display_menu_base` and `graphical_display_menu` are vendored, not forked

They live under `components/` as modified copies of the upstream components, taking the
pinned release as the base and re-applying JetHome features on top — rather than carrying
an old fork forward. That keeps the diff against upstream equal to the intentional additions
and nothing else (`196f65a`).

That property is only worth anything if it stays visible, hence the `JETHOME-BEGIN` /
`JETHOME-END` markers and `components/README.md`. **A re-sync starts by diffing against the
matching upstream tag**; if the markers have drifted, fix the markers before the code.

What is kept on top of upstream: `display_menu.back` / `back()`, `reset_menu()` /
`is_at_main()`, `right_for_menu_enter`, the `value` menu item type, and
`graphical_display_menu`'s `fill_row` / `restore_page` / `shrink_label` with the local
`shrink_text_to_width_()`.

## Menu item order is package order — there is no ordering knob

Upstream `display_menu_base` builds `items:` in the order they appear after package merge.
It has no `weight:` and no `position: {before:/after:}` (the JetHome fork does; that
machinery is not vendored here, and vendoring it means taking on the codegen sort, the
visibility pass and the runtime tree-mutation API with it).

So the only lever is where a menu package sits in the entry file's `packages:` list.
A package that must land in a specific slot inside an existing submenu cannot express that —
it appends. Plan the `packages:` order deliberately, and if a config ever genuinely needs
insertion by name, that is the moment to reconsider vendoring the ordering machinery, not
before.

## Each device entry file keeps its own `packages:` list

`include/jxd-r6-e1eth-base.yaml` carries what every JXD device shares; the entry file adds
exactly what its variant needs — the network package (`ethernet.yaml` or `wifi.yaml`), its
menu items, its display. This reads as duplication between the `-eth` and `-wifi` variants
and is not: the two differ in which packages they compose, and collapsing that into the base
would mean the base deciding for the device.

## QEMU overlays override only what the emulator cannot do

`scripts/qemu.sh` layers `include/features/qemu*.yaml` on top of the **real** device config,
included verbatim as a package; later packages win. There is deliberately no separate "QEMU
device config" to keep in sync, and the overlays never duplicate device logic.

Each override earns its place by being something QEMU physically cannot emulate:

- **`ethernet: type: OPENETH`** — the boards drive a LAN8720 PHY through the ESP32 EMAC,
  which QEMU has no model for. The virtual OpenCores MAC takes no PHY pins, and its schema
  rejects the LAN8720-only keys, hence the removals. Note it is `clk: !remove`, not
  `clk_mode:` — this repo migrated to the `clk:` block, and removing the old key would be a
  silent no-op that fails validation on the key that is actually there.
- **`i2c: scan: false`** — with no I²C controller behind it every address probe burns its
  full timeout, and a boot scan is ~112 of them in a row.
- **ADC sensors `update_interval: never`** — this one is fatal rather than noisy. The
  conversion never completes, so `ADCSensor::sample()` spins with interrupts off until the
  interrupt watchdog reboots the chip: a boot loop, not a failed component. The entities stay
  so the emulated device exposes the same set as the real one.
- **`fn_button` as a `template` sensor** — it is on GPIO0, which floats in emulation and,
  the pin being declared inverted, reads as held. Held for 10 s that means factory reset, so
  an untouched emulated device wipes its own flash about fifteen seconds into every boot.
- **DIO flash mode** — QEMU's flash model serves the ROM/bootloader path fine but hands the
  runtime driver garbage in QIO, which surfaces as filesystem corruption that never recovers.
- **Relays, inputs and the joystick as `template`** — they hang off PCA9554 expanders on
  I²C. Left alone they are not merely dead: the expander is re-probed on every read and
  floods the log. As templates they keep every id, name and automation, and can be driven
  over REST, so a feature that binds an input to a relay is testable.

Components on unemulated buses that only mark themselves failed are **left in the config on
purpose** — the emulated device then exposes the same entity set as the real one.

Only the `-eth` config is emulable: the `-wifi` one starts the radio from `on_boot`, and
Wi-Fi is not emulated.

## `virtual_display` exists so the display is testable without hardware

The panel is on I²C, so under emulation the real driver leaves the screen blank and no key
ever fires. `components/virtual_display` is the same 128×64 mono buffer served over HTTP
instead of pushed to a chip; pages, fonts, menus and every `on_press` automation are
untouched and render exactly as on hardware.

It is a QEMU-only component in practice, but it is a normal ESPHome display platform with no
QEMU dependency — nothing stops it being pointed at a real build for a screenshot.

It uses `request->url_to(buf)` rather than the deprecated `request->url()`, which upstream
removes in 2026.9.0. `url_to()` percent-decodes where `url()` did not; the key-name validator
is stricter than that needs and stays correct either way.

## CI compiles every config, it does not just validate them

`.github/workflows/build.yml` discovers `JXD/*.yaml` and `E1/*.yaml` at run time and runs a
full `esphome compile` per config in its own matrix job. `esphome config` is not enough: the
ESPHome API breaks that actually bite here surface as C++ compile errors and pass validation
cleanly (`dcc5cf4`).

`TZ` is pinned to a full IANA key because ESPHome bakes a timezone into the firmware and
falls back to the build machine's own when the config does not set one — and a bare `UTC` is
detected but then fails to load tzdata (`90a0500`).

Toolchain cache is keyed on the ESPHome pin, not on the config: it is ~2 GB and identical
across configs.

## Timezone comes from Home Assistant, and Home Assistant is the last time platform

Every time platform emits its own `set_global_tz()` at startup and the last one wins. With HA
last, the build machine's detected timezone no longer silently overrides the configured one.
Persistence across reboots is built on `get_global_tz()` / `set_global_tz()`, storing the
parsed POSIX form rather than a string, because `RealTimeClock::get_timezone()` is gone
(`6b76384`).
