# Release workflow

How firmware gets built, versioned and published, and what runs where.

## What runs automatically

| Workflow | When | What it does |
| --- | --- | --- |
| Build (`ci.yml`) | push to `dev` or `master`, every PR, manual | Discovers firmwares in `firmwares.yaml`, validates then compiles each with the pinned ESPHome, runs the component tests, verifies `dist/` is current, runs lint; `ci-ok` aggregates the lot into the one check branch protection requires |
| Release (`release.yml`) | a release is published (incl. prerelease), push to `dev` (nightly), manual dispatch | Compiles every firmware, uploads the `upload: true` ones to fw.jethome.com; GitHub releases are minted for published releases and dispatches only — dev pushes go to the nightly channel without a GitHub release |
| ESPHome release check (`esphome-release-check.yml`) | weekly, manual | On a new upstream ESPHome release: compiles every firmware with it and opens an issue with the results — the go/no-go for the dependabot bump |
| Draft release (`draft-release.yml`) | push to `master`, manual | Refreshes the rolling draft release tagged with the next version — publish it to build and ship |
| Dependabot | weekly | PRs bumping workflow actions and the pinned files in `requirements.txt` / `requirements-dev.txt`; an esphome bump PR is build-tested by Build |

## The firmware list: `firmwares.yaml`

The single source of truth for CI and releases. Each entry:

```yaml
firmwares:
  - config: devices/JXD/jxd-r6-e1eth-lcd.yaml  # device config, relative to repo root
    device: jxd-r6-e1eth-lcd               # slug level on fw.jethome.com
    upload: true                           # copy ota+factory to fw.jethome.com on release
```

Binaries of `upload: false` firmwares ship as GitHub release assets on
release events and non-dry-run dispatches — they never reach the firmware
server, and dev nightlies carry no GitHub release at all.
`scripts/firmware-matrix.py` validates the file and turns it into the
workflow matrices.

## Channels and versions

The firmware version is derived from the ESPHome pin in `requirements.txt`
(`<esphome>` below), not from the repository release tag:

| Channel | When | Version format | Example |
| --- | --- | --- | --- |
| `release` | full releases; manual dispatch with `channel: release` | `<esphome>.<sub>` | `2026.8.2.0` |
| `nightly` | push to `dev`; prereleases; manual dispatch (default channel) | `<esphome>.<YYYYMMDD>.<attempt>` | `2026.8.2.20260911.1` |

`<sub>` and `<attempt>` auto-increment from the existing git tags of previous
releases (max + 1), so no counter lives anywhere: a re-release of the same
esphome version bumps `<sub>`, and a new esphome version starts over at
`.0`. Nightly attempts per date start at `.1`. There is no beta channel on
fw.jethome.com yet; prereleases go to `nightly` until one exists.

The version reaches the firmware through its `version` substitution
(`esphome -s version <ver> compile ...`), so nothing in the configs is edited
at build time.

## Cutting a release

1. Make sure `requirements.txt` pins the esphome version you want to ship
   (dependabot opens the bump PR, Build CI test-builds it — just merge).
2. Merge `dev` into `master`: released firmware, `dashboard_import` and the
   asset URLs in `dist/` all come from `master`. The push refreshes the
   rolling draft release (below) tagged with the next version.
3. Open the draft release on GitHub and press **Publish release**. The tag
   triggers the Release workflow: every firmware is built, the binaries land
   on the release as assets, and the `upload: true` ones ship to
   fw.jethome.com.
4. No draft at hand, or need a rebuild of the same content? **Actions →
   Release → Run workflow**:
   - `dry_run` on: builds everything, uploads artifacts, touches nothing;
   - `dry_run` off and `channel: release` (the dispatch default is `nightly`
     — the safe side: it never moves the release channel's `latest` pointer):
     the workflow computes the version, creates the GitHub release, attaches
     all binaries, and uploads the `upload: true` firmwares to the server.
5. A full release tag must be `<esphome>` (workflow picks the next
   subversion) or `<esphome>.<sub>` — the esphome part must match the
   `requirements.txt` pin, otherwise the run fails.

## Draft releases

`draft-release.yml` keeps exactly one rolling draft on master: every push to
`master` deletes the stale draft (if any) and creates a fresh one, tagged
with the next release version (`scripts/release-version.py` — the same
computation the Release workflow's resolve step uses) and carrying
auto-generated notes. The draft is workflow-owned: manual edits to its notes
are overwritten by the next refresh. Publishing the draft creates the tag,
which is all the Release workflow needs to build and ship.

A manual `channel: release` dispatch that lands while the draft holds the
same number deletes that draft and publishes a real release itself — the
server is never fed from an unpublished draft.

## What lands where

**GitHub release assets** (release events, non-dry-run dispatches) — every
built firmware, both images; dev nightlies have no GitHub release:

```
<config-stem>-<version>-factory.bin   # merged image for flashing
<config-stem>-<version>-ota.bin       # OTA image
*.md5
```

**fw.jethome.com** — only `upload: true` firmwares, both images per device:

- hierarchy `JetHome.jxd.firmware.esphome.<device>.<channel>`
- image types `esp.bin` (factory) and `esp.ota` (OTA)
- hash: md5 (the server serves it as `info.md5` for OTA updates)
- `supported_devices`: the device slug; the `latest` pointer moves only on
  `release` channel uploads (manual runs control it with `update_latest`)

## Updates on the device

A firmware built here checks fw.jethome.com for a newer build of its own device
slug and installs it over HTTPS. Three entities:

| Entity | What it does |
| --- | --- |
| `Firmware update` | The version the server offers and the button that installs it; checks every 6 hours on its own |
| `Firmware channel` | `release` or `nightly` — the channel the check reads, kept across reboots |
| `Check for updates` | Checks now instead of waiting for the next poll; offline it does nothing |

The check reads `https://fw.jethome.com/api/devices/<device>/info`, where `<device>`
is the config's `fw_device` substitution — the slug from `firmwares.yaml`, not the
name the device was imported under — and offers the `esp.ota` image of the selected
channel whenever its version differs from the one the firmware was built with.
Prereleases go to `nightly`, full releases to `release`; picking a channel the
pipeline has never published to leaves the entity in an error state, with the
channel the manifest lacked named in the log.

The display carries the same under **Settings → Firmware**: the running and the offered
version, the channel, a check, and an install behind a confirmation. An install started
anywhere — that row, Home Assistant, the dashboard — takes the screen over until the device
reboots into the new firmware: the version being written, the percentage and a progress bar,
and no blanking while it runs. An install that fails says so on the same screen, leaves the
running firmware in place, and gives the screen back on the next button press.

The wiring is `devices/JXD/packages/features/firmware-update.yaml` with its menu rows in
`devices/JXD/packages/display/menu-firmware.yaml` and its install screen in
`devices/JXD/packages/display/firmware-page.yaml`, and all three are left out of `dist/`: a
firmware built from the imported config is the user's own, not one this pipeline publishes
([dist/ and new devices](DIST.md)).

## Secrets

| Secret | Purpose |
| --- | --- |
| `FWSITE` | Firmware server base URL (`https://fw.jethome.com`). The upload action defaults to its test host, so the workflow refuses to run without it. |
| `FWUPLOAD` | Upload token for the firmware server. |

Both are organization secrets; they are never used outside the `upload-fw`
job.

## Local equivalent

```bash
esphome -s version <version> compile <config.yaml>
# images land in <config-dir>/.esphome/build/<name>/build/firmware.{factory,ota}.bin
```
