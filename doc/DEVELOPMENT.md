# Development

Setting up, building, and the checks that gate a push.

## Environment

`scripts/setup.sh` (`scripts\setup.bat` on Windows) creates `.venv/`, installs `requirements.txt`
and `requirements-dev.txt`, and installs the pre-commit hook. ESPHome is pinned to 2026.8.2;
Python 3.12–3.14. Activate the venv before committing: the hooks run with whatever `python3`
git sees.

```bash
./scripts/setup.sh && source .venv/bin/activate
```

## Commands

```bash
esphome config  devices/JXD/jxd-r6-e1eth-lcd.yaml                 # validate only, no toolchain
esphome compile devices/JXD/jxd-r6-e1eth-lcd.yaml                 # full build
esphome run     devices/JXD/jxd-r6-e1eth-lcd.yaml --device <ip>   # build + OTA (omit --device for USB)
esphome -s version 2026.8.2.0 -s timezone Europe/Berlin compile <config>   # substitutions from the CLI

python scripts/build-dist.py  [--check]    # regenerate / verify dist/ (imports esphome: use the venv)
python scripts/build-icons.py [--check]    # regenerate / verify assets/res/
python scripts/firmware-matrix.py build    # CI matrix from firmwares.yaml; also validates the file
python tests/host/run.py                   # build the automations engine for the host and run it
python tests/unit/run.py                   # build the automations unit tests for the host and run them

pre-commit run --all-files                 # ruff --fix, ruff-format, pyupgrade --py310-plus, yamllint, clang-format, build-icons, build-dist
SKIP=build-dist pre-commit run --all-files # what the CI lint job runs

./scripts/qemu.sh run jxd-r6-e1eth-lcd --daemon --wait-http 240   # boot it in QEMU, see doc/QEMU.md
./scripts/qemu.sh stop                                            # and shut it down again

.venv/bin/python scripts/modbus_probe.py --port /dev/ttyUSB2 probe   # walk the Modbus map over RS485
```

Build output lands next to the config:
`devices/JXD/.esphome/build/<name>/.pioenvs/<name>/firmware.{factory,ota}.bin`.

CI compiles with `TZ=Etc/UTC`: the `homeassistant` time platform bakes the build host's zone into
the firmware.

## Before pushing

The Build workflow is these five; run them locally:

1. `esphome compile` for every config in `firmwares.yaml`
2. `python tests/host/run.py`
3. `python tests/unit/run.py`
4. `python scripts/build-dist.py --check`
5. `pre-commit run --all-files`

Both suites build `components/automations` for the ESPHome `host` platform, so neither needs an
ESP toolchain. `tests/host/` runs the engine against the rule files in `tests/host/fixtures/`
and takes about a minute. `tests/unit/` is a Google Test binary over the engine's classes and
takes seconds once googletest is built. Behavior and usage: [AUTOMATIONS.md](AUTOMATIONS.md).

## Generated files

- `assets/res/*.svg` — `scripts/build-icons.py`, from the 16×16 pixel maps in that script. Edit
  the maps, never the SVGs.
- `dist/<device>.yaml` — `scripts/build-dist.py`; regenerate and commit after changing anything a
  device config pulls in. Excluded from yamllint. Rules: [DIST.md](DIST.md).

`firmwares.yaml` must list every device config (`build-dist.py` refuses to run otherwise); it is
the only input to the Build and Release matrices.

## Bumping ESPHome

The pin appears in these files; Dependabot bumps only the first:

- `requirements.txt` — the source of truth: firmware versions and CI cache keys derive from it
- `.pre-commit-config.yaml` — `additional_dependencies` of the `build-dist` hook
- `README.md` — badge and text
- `scripts/setup.sh`, `scripts/setup.bat` — the Python-range messages
- `doc/DEVELOPMENT.md` (Environment) and `CLAUDE.md` — the stated version

After a bump, re-check every entry under "Coupled to upstream internals" in
[ARCHITECTURE.md](ARCHITECTURE.md): that list is the one that grows, this one would go stale.

Also re-sync `.clang-format` and the `mirrors-clang-format` rev in `.pre-commit-config.yaml` with
upstream's: a bump can change either the style config or the version it formats with.

## Branches

`dev` is the default branch: pull requests target it, and Build runs on every push to it.
`master` is the release branch — `dashboard_import` and the asset URLs in `dist/` point at
`@master`, so it moves only when `dev` is merged into it for a release. The
`no-commit-to-branch` hook keeps commits off `master`.

## Style

- YAML: yamllint — 2-space indent, indented sequences, at most one blank line, no `---`, no
  line-length limit.
- Python: ruff defaults plus `ruff format`, `pyupgrade --py310-plus`; standalone scripts, no
  `pyproject.toml`.
- C++: clang-format over `components/`, using upstream ESPHome's `.clang-format` verbatim so the
  components read like the components they live next to. pre-commit reformats in place; to run it by
  hand, `pre-commit run clang-format --all-files`.
- Comments say why in a line or two; the longer story goes in the commit message. README and
  `doc/` state behavior and usage, not mechanism.
