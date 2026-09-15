# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

ESPHome configurations for JetHome JXD devices. A device is a thin YAML under `devices/` that
lists packages; the logic is C++ lambdas inside those packages; `scripts/` holds the Python
tooling. ESPHome is pinned to 2026.8.2. There is no test suite: CI compiles every firmware in
`firmwares.yaml`, checks the generated files and runs the pre-commit hooks.

## Where to look

| Before you… | Read |
| --- | --- |
| build, flash, lint, regenerate, push, bump ESPHome, or wonder about style | [doc/DEVELOPMENT.md](doc/DEVELOPMENT.md) |
| edit a package: shared ids, boot order, settings, temperature slots, Modbus map, upstream-coupled code | [doc/ARCHITECTURE.md](doc/ARCHITECTURE.md) |
| touch `dist/`, `dashboard_import`, assets, or add a device | [doc/DIST.md](doc/DIST.md) |
| touch CI, firmware versions, channels, publishing | [doc/RELEASE.md](doc/RELEASE.md) |
| change how Dallas sensors get their slots | [doc/ONEWIRE_WORKFLOW.md](doc/ONEWIRE_WORKFLOW.md) |
| change WiFi provisioning | [doc/WIFI_SETUP.md](doc/WIFI_SETUP.md) |

The tree itself is in the README, "Repository Layout".

## Always

- `dist/` and `assets/res/` are generated (`scripts/build-dist.py`, `scripts/build-icons.py`).
  Never edit them by hand; regenerate and commit them with the change that made them stale.
- Don't read `dist/`: it is only the flattened copy of `devices/`, so everything in it is in
  the sources. Work from those.
- Comments say why in a line or two; the longer story goes in the commit message. README and
  `doc/` state behavior and usage, not mechanism.
