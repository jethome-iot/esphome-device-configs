# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ESPHome configurations for JetHome JXD devices. A device is a thin YAML under `devices/` that
lists packages; the logic is C++ lambdas inside those packages and the external components
under `components/`; `scripts/` holds the Python tooling. ESPHome is pinned to 2026.8.2. CI
compiles every firmware in `firmwares.yaml`, runs the test suites in `tests/`, checks the
generated files and runs the pre-commit hooks.

## Where to look

| Before you… | Read |
| --- | --- |
| build, flash, lint, regenerate, push, bump ESPHome, or wonder about style | [doc/DEVELOPMENT.md](doc/DEVELOPMENT.md) |
| edit a package: shared ids, boot order, settings, temperature slots, Modbus map, upstream-coupled code | [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) |
| touch `dist/`, `dashboard_import`, assets, or add a device | [doc/DIST.md](doc/DIST.md) |
| touch CI, firmware versions, channels, publishing | [doc/RELEASE.md](doc/RELEASE.md) |
| run a device config in the emulator, or touch the QEMU overlays | [doc/QEMU.md](doc/QEMU.md) |
| change how Dallas sensors get their slots | [doc/ONEWIRE_WORKFLOW.md](doc/ONEWIRE_WORKFLOW.md) |
| use or change the HTTP file API over the user partition | [components/web_file_browser/README.md](components/web_file_browser/README.md) |
| touch the `automations` engine or its rule format | [doc/AUTOMATIONS.md](doc/AUTOMATIONS.md) |
| add a test, or wonder which suite a case belongs in | [doc/TESTING.md](doc/TESTING.md) |
| change what a relay or an input remembers across reboots | [doc/ENTITY_SETTINGS.md](doc/ENTITY_SETTINGS.md) |
| change WiFi provisioning | [doc/WIFI_SETUP.md](doc/WIFI_SETUP.md) |

The tree itself is in the README, "Repository Layout".

## Always

- `dist/` and `assets/res/` are generated (`scripts/build-dist.py`, `scripts/build-icons.py`).
  Never edit them by hand; regenerate and commit them with the change that made them stale.
- Don't read `dist/`: it is only the flattened copy of `devices/`, so everything in it is in
  the sources. Work from those.
- A component under `components/` ships with tests wherever its code allows it, and there is
  always something. A schema or a validator is testable from any component, ESP-IDF-bound or
  not: assert that a bad config is refused with the right message, not only that a good one
  passes. Everything else goes under `tests/`, built for ESPHome's host platform; which suite
  a case belongs in is in [doc/TESTING.md](doc/TESTING.md). Only code that exists solely on
  ESP-IDF (`web_server_base`, NVS, LittleFS) is out of reach there; say so in the pull request
  instead of leaving the gap unexplained.
- Comments say why in a line or two; the longer story goes in the commit message. README and
  `doc/` state behavior and usage, not mechanism.
