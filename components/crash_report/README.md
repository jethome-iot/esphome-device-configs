# crash_report

Keeps the record ESPHome's crash handler takes of a panic as a text file on a
`filesystem_storage_abstract` mount, so a device in the field hands it back over HTTP instead of
printing it to a serial console nobody is attached to. ESP32 on ESP-IDF only, and it needs
`logger:`.

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/jethome-iot/esphome-device-configs
      ref: master
      path: components
    components: [filesystem_storage_abstract, littlefs_storage, crash_report]

littlefs_storage:
  id: user_storage
  partition_label: littlefs
  partition_size: 4MB
  base_path: /littlefs

crash_report:
  storage: user_storage
```

## Options

| Option       | Default | Meaning |
| ------------ | ------- | ------- |
| `storage`    |         | Required. The mount to write to, such as `littlefs_storage` |
| `report_dir` | `crash` | One folder below the mount's base path; a name holding `/` or `..` is refused |
| `keep`       | `4`     | Reports kept, `1` to `16` |

## When a report is written

At the boot after a panic — a fault, an `abort()` or a failed assert, or a watchdog that fired.
The record lives in RAM that survives a reset but not a power cut, so a device that lost power
before it came back up has nothing to write. The report is written right after the mount comes
up, and the record is then marked as taken, so the next reboot does not write it again; the
boot log and an `esphome logs` client that connects during this boot still show it. A mount that
is not there or a write that fails leaves the record in RAM, and the next reboot tries again,
unless an `esphome logs` client connected in between and took it.

The newest report is `crash0.txt` in `report_dir`. Each new one moves the others up a number,
to `crash<keep-1>.txt`, and drops the one already there. A factory reset that formats the
partition takes the reports with it.

## The file

Plain ASCII. Three lines name the firmware that wrote the file, then comes ESPHome's record as
the boot log shows it, without the log prefix and the colours:

```
firmware: JetHome.JXD-R6-E1ETH-LCD 2026.9.0.0
build: 2026-09-29 12:28:56 +0000
elf_sha256: f6efc3d6b4d1f4a5b465e44e95c592620984c0663605da26a49c5565074bb0f4
*** CRASH DETECTED ON PREVIOUS BOOT ***
  Reason: Fault - LoadProhibited (cause 28)
  Crashed core: 1
  PC:  0x400D2A1B  (fault location)
  EXCVADDR: 0x00000010  (faulting address)
  BT0: 0x400D2A18  (backtrace)
  BT1: 0x400E51C4  (backtrace)
  Other core (0) backtrace:
  BT0: 0x40081234  (backtrace)
Use: addr2line -pfiaC -e firmware.elf 0x400D2A1B 0x400D2A18 0x400E51C4
Other core: addr2line -pfiaC -e firmware.elf 0x40081234
```

- `firmware:` the project name and version; without `esphome.project`, the device name and
  the ESPHome version
- `build:` when that firmware was built
- `elf_sha256:` what `sha256sum` prints for that build's `firmware.elf`

`Reason` is the kind of crash; the faulting address is there for a fault only. Each core gets up
to 16 frames. The record says where the code was, not which task ran or how much heap was left.

A record taken by a different build from the one that wrote the file — an update that crashed
and was rolled back, say — says `Captured by a different firmware build` and lists the addresses
in lowercase with no `addr2line` hint. They belong to the ELF of the build that crashed, which
the first three lines do not name.

## Fetching and decoding

The reports are files on the mount, so [`web_file_browser`](../web_file_browser/README.md)
serves them, behind the web server's credentials:

```sh
curl --digest -u admin:admin 'http://<device>/files/download?path=/crash/crash0.txt'
scripts/device-files.py get -r /crash ./crash
```

`crash-decode.py` from [jxd-devices-utils](https://github.com/jethome-iot/jxd-devices-utils)
turns the addresses into functions and source lines. It needs the `firmware.elf` of the build
the first line names, which `esphome compile` leaves next to the device config in
`.esphome/build/<name>/build/`; an ELF whose `sha256sum` matches the `elf_sha256` line is
exactly that build.

## Tests

`tests/components/crash_report/` covers the configuration refusals and, on the host with a
stand-in for ESPHome's crash handler, the rest: the text taken from each formatted log line and
the lines of other tags left out, the header, a worst-case dual-core record kept whole, the
rotation, and the record kept for the next boot by every failure — no mount, a folder or a file
that cannot be created, a write that fails at close. Out of reach there are the calls into the
crash handler and the ELF hash read from the app description, which exist only on ESP32; the
header the tests check has no `elf_sha256` line.
