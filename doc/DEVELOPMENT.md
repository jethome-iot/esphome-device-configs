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
python scripts/vendored-diff.py            # what components/ changed in the ESPHome components it shadows
python scripts/vendored-diff.py --check    # and whether every hunk of that is still marked
python tests/run.py [component]            # build every tests/components/*/ suite for the host and run it

pre-commit run --all-files                 # ruff --fix, ruff-format, pyupgrade --py310-plus, yamllint, clang-format, build-icons, build-dist
SKIP=build-dist pre-commit run --all-files # what the CI lint job runs

./scripts/qemu.sh run jxd-r6-e1eth-lcd --daemon --wait-http 240   # boot it in QEMU, see doc/QEMU.md
./scripts/qemu.sh stop                                            # and shut it down again

.venv/bin/python scripts/modbus_probe.py --port /dev/ttyUSB2 probe   # walk the Modbus map over RS485
```

Build output lands next to the config:
`devices/JXD/.esphome/build/<name>/build/firmware.{factory,ota}.bin`.

CI compiles with `TZ=Etc/UTC`: the `homeassistant` time platform bakes the build host's zone into
the firmware.

## Before pushing

The Build workflow is these; run them locally:

1. `esphome config` for every config in `firmwares.yaml` — the fast schema gate CI runs as the
   `Validate` jobs; `esphome compile` subsumes it locally (a config that compiles validates)
2. `esphome compile` for every config in `firmwares.yaml`
3. `python tests/run.py`
4. `python scripts/build-dist.py --check`
5. `pre-commit run --all-files`

The tests need no ESP toolchain. What they cover and how to add one: [TESTING.md](TESTING.md).

CI aggregates everything into the single `ci-ok` check, which is what branch protection
requires.

## Generated files

- `assets/res/*.svg` — `scripts/build-icons.py`, from the 16×16 pixel maps in that script. Edit
  the maps, never the SVGs.
- `dist/<device>.yaml` — `scripts/build-dist.py`; regenerate and commit after changing anything a
  device config pulls in. Excluded from yamllint. Rules: [DIST.md](DIST.md).
- `components/web_device_dashboard/dashboard_index.h` — the gzipped dashboard page.
  `npm run build` in [jethome-devices-web-dashboard](https://github.com/jethome-iot/jethome-devices-web-dashboard)
  writes `dist/dashboard_index.h`; copy it over this one, or merge the pull request that
  repository's Release workflow opens.

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

`components/display_menu_base` and `components/graphical_display_menu` replace the ESPHome
components of those names, so a bump does not reach them. Before changing the pin, save
`scripts/vendored-diff.py` output; after it, copy both directories from the new ESPHome and
re-apply that patch. Every hunk it prints is marked `JetHome:` in the source, which is what the
`vendored-diff` pre-commit hook checks — it fails on the un-re-copied tree, so the pin in that
hook has to move with the others.

Also re-sync `.clang-format` and the `mirrors-clang-format` rev in `.pre-commit-config.yaml` with
upstream's: a bump can change either the style config or the version it formats with.

## Branches

`master` is the source of truth: `dashboard_import` and the asset URLs in `dist/` point at
`@master`, and a release is built from it. `dev` is the default branch and where development
happens — most pull requests target it, and it reaches `master` when it is merged in for a
release.

Some pull requests target `master` directly, and the reason is always the same: the change has
to be true on `master` before the next release rather than after it. The release workflows
themselves, whatever only affects what `master` publishes, and the rules everything else is
held to. `master` is then merged back into `dev` with a merge commit, so the two never drift
and nothing has to be applied twice.

Build runs on every push to either branch, and everything lands through a pull request.

## Issues and pull requests

Every change starts as an issue — a feature, a bug, a refactor, a documentation fix, a chore.
The issue is where the work is described and agreed; the pull request only carries it out. A
review finding that is not fixed in the pull request it was raised on is filed as its own issue
before that pull request is called done, so nothing real is left behind in a comment thread.

A milestone is an **epic issue**, labelled `type:epic` — not the GitHub Milestones feature. It
carries the list of its children as a task list, and each child links back to it. The state of
a milestone is then one page, and it is the same kind of object as everything else, so it takes
discussion and links like everything else.

Every pull request opens on an issue and says which one in its body:

| | |
| --- | --- |
| `Closes #N` | this pull request finishes the issue |
| `Part of #N` | one of several; the issue stays open |

GitHub acts on those keywords only when the pull request targets the default branch. On one
that targets `master` the line still records which issue the work belongs to, but nothing
closes the issue: that is done by hand when the pull request merges, because the later merge
into `dev` never reconsiders a pull request body it did not carry.

A pull request with nothing behind it is one nobody agreed to. When something else turns out to
need doing mid-change and does not belong in the change at hand, file it and link it instead of
widening the pull request.

Templates for all three kinds of issue, and for the pull request, are in `.github/`. GitHub
reads them from the default branch only, so a change to them takes effect when `dev` has it,
not when `master` does.

### Labels

Two axes, each with its own prefix so the list groups them and neither is mistaken for the other.

**`type:`** — one per issue. The bug and epic templates carry theirs, so an issue filed from
either arrives with it. The task template covers the four kinds that are left and cannot: a
label is set at filing only by someone with triage access, which an outside contributor does
not have. So on those, the type is whatever the author could set, and otherwise the first thing
whoever triages the issue does.

| | |
| --- | --- |
| `type:bug` | behaves differently from what it says it does |
| `type:feature` | something the firmware or the tooling cannot do yet |
| `type:refactor` | the same behaviour in a better shape |
| `type:docs` | the README, `doc/`, a component's README, comments |
| `type:chore` | CI, dependencies, tooling, housekeeping |
| `type:epic` | a milestone, above |

**`status:`** — at most one, and only for what nothing else records. An issue carrying none is
one nobody has looked at yet.

| | |
| --- | --- |
| `status:needs-decision` | waiting on a developer to decide something |
| `status:ready` | decided and described; anyone can take it |
| `status:blocked` | waiting on something outside this repository |

There is deliberately no label for in progress, in review or done. An assignee, a linked pull
request and a closed issue already say those three, and a label that repeats what something else
records is a label that can come to disagree with it.

`status:needs-decision` is the one with teeth:

- Anyone may set it, but the issue then has to say **what** is being decided and what the
  options are. A gate with no question inside it is a stall.
- Only a developer takes it off, and the way to take it off is to write the decision in the
  issue — so the decision is on the record where the work is, not in a chat.
- **No pull request opens on an issue that carries it.** That rule is what makes the label mean
  anything; without it the label is a sticker.

The remaining labels say nothing about what an issue is or where it stands: `dependencies`,
`github_actions` and `python` are Dependabot's, `ready-to-merge` says a pull request's review
converged, and `good first issue` and `help wanted` are the two GitHub itself surfaces.

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
- English, everywhere it is written down: issues, pull requests, commit messages, code comments,
  the README and `doc/`. Whatever language a discussion happens in, what lands in the repository
  is in one language.
