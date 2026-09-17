# Release workflow

How firmware gets built, versioned and published, and what runs where.

## What runs automatically

| Workflow | When | What it does |
| --- | --- | --- |
| Build (`build.yml`) | push to `dev` or `master`, every PR, manual | Discovers firmwares in `firmwares.yaml`, compiles each with the pinned ESPHome, verifies `dist/` is current, runs lint |
| Release (`release.yml`) | a release is published (incl. prerelease), manual dispatch | Compiles every firmware, attaches binaries to the GitHub release, uploads the `upload: true` ones to fw.jethome.com |
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

Binaries of `upload: false` firmwares still ship as GitHub release assets —
they just never reach the firmware server. `scripts/firmware-matrix.py`
validates the file and turns it into the workflow matrices.

## Channels and versions

The firmware version is derived from the ESPHome pin in `requirements.txt`
(`<esphome>` below), not from the repository release tag:

| Channel | When | Version format | Example |
| --- | --- | --- | --- |
| `release` | full releases; manual dispatch with `channel: release` | `<esphome>.<sub>` | `2026.8.2.0` |
| `nightly` | prereleases; manual dispatch (default channel) | `<esphome>.<YYYYMMDD>.<attempt>` | `2026.8.2.20260911.1` |

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

**GitHub release assets** — every built firmware, both images:

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
# images land in .esphome/build/<name>/.pioenvs/<name>/firmware.{factory,ota}.bin
```
