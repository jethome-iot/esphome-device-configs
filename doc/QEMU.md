# Running device firmware in QEMU

Boots a **real** JXD device config in Espressif's QEMU fork, with working
networking and a browser front panel for the LCD. The emulated device serves its
web server and REST API on a forwarded host port and keeps its flash state (NVS,
LittleFS) across restarts — no hardware involved.

```bash
scripts/qemu.sh run jxd-r6-e1eth-lcd-eth --daemon --wait-http 240
# → http://127.0.0.1:8080         device web server
# → http://127.0.0.1:8080/panel   the 128x64 screen and its joystick
scripts/qemu.sh stop                                    # when you are done
```

Only `jxd-r6-e1eth-lcd-eth` is emulable. `-wifi` runs `set_wifi_ap` from
`on_boot` and QEMU implements no radio, so the script refuses that device by name.

## Prerequisites

Espressif's QEMU fork — upstream `qemu-system-xtensa` has no `esp32` machine:

```bash
IDF_PATH=~/.platformio/packages/framework-espidf \
  python3 ~/.platformio/packages/framework-espidf/tools/idf_tools.py install qemu-xtensa
sudo apt install libslirp0     # user-mode networking; the binary will not start without it
```

The script picks the newest install under `~/.espressif/tools/qemu-xtensa/` and
refuses an upstream build; `QEMU_XTENSA=...` overrides discovery. Compiling uses
the repo venv (`requirements.txt`), which the script activates itself.

## Commands

```bash
scripts/qemu.sh list                       # device configs, marking the running ones
scripts/qemu.sh run <device>               # compile, build the flash image, boot
scripts/qemu.sh run <device> --no-build    # boot what was built last
scripts/qemu.sh run <device> --fresh       # wipe emulated flash first (factory device)
scripts/qemu.sh build <device>             # compile only
scripts/qemu.sh image <device>             # (re)create the padded flash image only
scripts/qemu.sh stop [<device>]            # stop instances, keep flash state and build
scripts/qemu.sh clean [<device>]           # stop instances, drop wrappers and flash images
```

Options: `--http-port` (default 8080), `--api-port` (6053), `--ota-port` (3232),
`--psram 2M|4M|none`, `--no-wdt`, `--daemon`, `--wait-http <sec>`. Exit an
interactive session with `Ctrl-A x`. Each instance holds all three forwarded
ports, so a second one needs its own trio — a clash is caught before QEMU starts
and names the port:

```bash
scripts/qemu.sh run <device> --http-port 8091 --api-port 6064 --ota-port 3243 --daemon
```

### Stop what you started

A `--daemon` instance outlives the session that started it, and it is not idle
scenery: it keeps its ports and keeps writing back into its flash image. The next
`run` of the *same* device clears it away; anything else just sees ports taken.
`stop` leaves the build and the flash alone, so the device comes back with its
saved state via `run --no-build`; `clean` throws the flash away too.

## How a real config gets into QEMU

No QEMU-specific device config is maintained. `scripts/qemu.sh` generates a
throwaway wrapper — `JXD/<device>.qemu.yaml`, gitignored — whose `packages:` are
the real config, then `include/features/qemu.yaml`, then
`include/features/qemu-<device>.yaml` if it exists. Packages merge in order and
later ones win, so the overlays override the device; `!remove` drops keys the new
schema rejects and `!extend` patches list entries by id.

The wrapper has to sit in `JXD/` — `../fonts/*.bdf` and `../components` resolve
against the *main* config's directory. The build is renamed `<device>-qemu` so
the emulator gets its own build directory instead of forcing a full ESP-IDF
rebuild on every switch.

## What the overlays change, and why

`include/features/qemu.yaml` — only what QEMU physically cannot do:

| Override | Reason |
|---|---|
| `ethernet: type: OPENETH`, every PHY key removed | The board drives a LAN8720 through the ESP32 EMAC, which QEMU has no model for. OPENETH is the virtual OpenCores MAC behind `-nic model=open_eth`, and its schema takes no PHY keys — including `clk:`, so removing `clk_mode:` instead would be a silent no-op that fails validation. |
| `i2c: scan: false` | No I2C controller, so every address probe burns its full timeout — a boot scan is ~112 in a row. |
| `esp32: flash_mode: dio` | QEMU's flash model serves the bootloader path fine but hands the runtime driver garbage in QIO, so LittleFS reports "Corrupted dir pair" and format never succeeds. |
| `vin_meas` / `poe_voltage`: `update_interval: never` | **Fatal otherwise.** No ADC, and the conversion never completes: `ADCSensor::sample()` spins with interrupts off until the interrupt watchdog reboots the chip — a boot loop, not a failed component. The entities stay (`input_voltage` reads both) and just never sample, so the screen shows `VIN: nanV`. |
| `fn_button` → `template` | FN is GPIO0, which floats and is declared inverted, so it reads as permanently held. No automation is bound to it today, so that alone is only a stuck entity — but the front panel injects `fn` as a key, and a `gpio` sensor re-reads the pin every loop and overwrites the injected state before any automation sees it. |
| `esphome: name_add_mac_suffix: false` | QEMU burns no eFuse MAC, so the suffix is `-000000` everywhere; it also costs 7 of the 31 hostname characters the `-qemu` rename needs. |
| `logger: logs: esp-idf: DEBUG` | ESPHome forwards every ESP-IDF message at DEBUG under the `esp-idf` tag whatever its own severity, and the device pins that tag to INFO — hiding exactly the driver errors that explain an emulation problem. |

`include/features/qemu/` replaces what hangs off the I2C expanders, pulled in per
device by `include/features/qemu-<device>.yaml`:

| Overlay | |
|---|---|
| `board-d6-r6.yaml` | The six relays and six inputs become `template`, keeping every id, name and automation — a relay then toggles over REST and holds. A `template` input cannot change on its own, so six `internal:` switches publish onto them. With nothing left pointing at the expander it is removed too, which is where most of the log noise went. |
| `panel-jxd-display.yaml` | The panel becomes a [`virtual_display`](../components/virtual_display/README.md) and the joystick becomes `template` sensors the front panel publishes into. |

## Talking to the emulated device

The web server here has no auth. Note that ESPHome 2026.8.2 matches REST entities
by **name**, not object id, so the path segment is the display name URL-encoded,
and the ESP-IDF httpd rejects a POST with no `Content-Length` (`-d ""`):

```bash
curl -s   'http://127.0.0.1:8080/switch/Relay%201'                     # {"id":"switch/Relay 1",...}
curl -sX POST -d "" 'http://127.0.0.1:8080/switch/Relay%201/turn_on'
curl -sX POST -d "" 'http://127.0.0.1:8080/switch/Drive%20input%203/turn_on'   # raises Input 3
```

## The front panel

The device serves its screen at `/panel`: the 128x64 canvas, with the joystick as
on-screen buttons (clickable, or arrows/Enter/Escape on the keyboard). It is the
real UI — the same pages, fonts and `graphical_display_menu` the hardware runs,
rendering into a framebuffer served over HTTP instead of pushed at a chip, with
keys injected into the binary sensors the joystick drives, so every `on_press`
fires exactly as on the device.

```bash
curl -s http://127.0.0.1:8080/panel/info                # {"width":128,"height":64,"keys":[...]}
curl -s http://127.0.0.1:8080/panel/frame -o frame.bin  # 1024 bytes, 1bpp
curl -sX POST -d "" http://127.0.0.1:8080/panel/key/down   # ?action=down|up to hold and release
```

A frame is `((width + 7) / 8) * height` bytes: pixel `(x, y)` is bit `7 - (x & 7)`
of byte `y * 16 + (x >> 3)`. Endpoints and options are in the
[component README](../components/virtual_display/README.md).

QEMU cannot help here: the `esp32` machine models no I2C controller at all, so
the SH1106 and the joystick expander have nothing to talk to.

## What works and what is dead

| Works | Dead in QEMU |
|---|---|
| `web_server` and the REST API, over the real device's entity set | TMP102 (PCB Temp) and the PCF8563 RTC — time comes from SNTP only |
| The display and its keys, via `/panel` | ADC (`vin_meas`, `poe_voltage` — never sampled) |
| Relays and inputs — state flips over REST and holds | 1-Wire (the bus reads as held low, no devices) |
| Ethernet on 10.0.2.15, native API on 6053, OTA, outbound HTTPS | Wi-Fi and BT; Modbus sees an emulated UART with nothing on it |
| NVS and `preferences`; flash state survives restarts | |

Two traps worth knowing:

- **The MAC is all zeros.** QEMU burns no eFuse MAC, so the base MAC reads
  `00:00:00:00:00:00`, the Ethernet MAC `00:00:00:00:00:03`, and anything keyed
  off the MAC sees the same value everywhere. Every read also logs
  `Failed to read a valid MAC address from eFuse`, which mDNS does constantly —
  that one line is the bulk of a run's log.
- **A software reset is where QEMU regularly falls over**, panicking right after
  the bootloader loads the app. The same image boots fine from a cold start —
  `stop`, then `run --no-build`.

## Flash state

The flash image (`JXD/.esphome/build/<device>-qemu/qemu-flash.bin`) is a full
16 MB image padded with `0xFF`, and QEMU writes back into it, so NVS keys and
LittleFS files survive restarts. It is rebuilt from the firmware whenever that
firmware is newer, so **any rebuild starts from blank flash**; `--fresh` forces
the same on demand, and `run --no-build` keeps state across a code change. The
padding matters — `truncate` would pad with zeros and leave NVS and LittleFS
reading garbage instead of blank flash.
