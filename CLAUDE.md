# JetHome ESPHome device configs

Firmware YAML for JetHome JXD devices. **Standalone repo** — it builds against plain
upstream ESPHome, pinned in `requirements.txt`, with no fork and no submodules.

| Path | Role |
|------|------|
| `JXD/jxd-<device>.yaml` | Device entry file, one per firmware target. `substitutions:` + `packages:` + what is genuinely device-specific. |
| `include/*.yaml` | The packages every entry file composes: board pin maps, display pages, menu items, peripherals, features. |
| `components/` | Vendored ESPHome components, pulled in per config via `external_components: {type: local, path: ../components}`. |
| `scripts/qemu.sh` | Boot a real device config in QEMU — see below. |
| `doc/DECISIONS.md` | Why things are the way they are. Prose lives here, not in the source. |

`components/display_menu_base` and `components/graphical_display_menu` are **modified
copies of upstream components**, tracking the version in `requirements.txt`. Every local
deviation is bracketed with `JETHOME-BEGIN`/`JETHOME-END` markers — keep them accurate,
they are the only thing making the next upstream re-sync tractable
(`components/README.md`).

## Every task that changes files runs in its own worktree

The main checkout is not where work happens:

```bash
git -C /home/alexey/dev/esphome-device-configs worktree add -b <type>/<slug> \
    ~/dev/wt-esphome-device-configs/<slug> master
ln -s <a venv matching requirements.txt> ~/dev/wt-esphome-device-configs/<slug>/.venv
```

`<type>` is `feature` / `fix` / `chore` / `docs` / `ci`. Then move the session into it
(`EnterWorktree` with that path) so edits and subagents land there.

**Check the venv before you build.** `.venv/` is gitignored, so a checkout's venv drifts
from `requirements.txt` silently and you end up building a different ESPHome than CI does.
`./.venv/bin/esphome version` must equal the pin.

**Removing the worktree is the user's call.** Commit, push, open the PR, report — and stop.

## Delegate subtasks to subagents

Independent pieces of work — porting a file, inventorying a component's deltas, tracing a
fault, analysing a PR — go to subagents running in their own worktrees, in parallel. Give
each the absolute worktree path, the file list and the questions to answer, and require
`file:line` in the answer: their tool output never reaches the user, so an unsourced claim
cannot be checked. The final judgement stays in the main thread.

## Device UI is verified on the emulator, not by reading code

Any device config boots in QEMU with working networking — the real firmware, built from the
real config, no hardware:

```bash
ss -ltnp | grep -E ':(8080|6053|3232)'      # free? another worktree may hold them
./scripts/qemu.sh run jxd-r6-e1eth-lcd-eth  # → http://127.0.0.1:8080
```

Devices with the display board also serve a **front panel** at `/panel`: the 128×64 screen
as a canvas plus the joystick as clickable keys, driving the same pages and
`graphical_display_menu` the hardware runs. Both are scriptable, so a UI change is checked
without a browser:

```bash
curl -s http://127.0.0.1:8080/panel/frame -o frame.bin      # 1024 B, 1bpp
curl -sX POST http://127.0.0.1:8080/panel/key/enter
```

**Whenever you touch a device's web interface, REST API, display pages or menus, boot the
affected config, look at the running result, and shut the emulator down when you are done**
(`./scripts/qemu.sh stop`; `list` shows what is still up). A `--daemon` instance outlives
the session and keeps its three forwarded ports. A busy port makes `run` refuse and exit
**0**, so a script that ignores the output happily talks to someone else's device.

What the emulator cannot show is any value that comes off a chip — I²C, SPI and the ADC are
not emulated, so board info, RTC and temperatures are absent rather than wrong. Wi-Fi is not
emulated either: only the `-eth` config is a viable target. Full picture: `doc/QEMU.md`.

## PR workflow: review before push, Copilot after, merge only on request

- **Review before every push.** Before pushing a branch that opens or updates a PR, run a
  **read-only** review subagent over `git diff master...HEAD` and fix or surface its findings
  first. Read-only is not a formality: a review agent with write access once ran
  `git stash -u` and wiped a live working tree mid-review. Use `Explore` or `/code-review` —
  never a general-purpose agent with default (write-capable) tools.
- **Request a Copilot review on every PR, then triage it.** `gh pr edit --add-reviewer`
  won't take the bot and the REST endpoint silently no-ops; use GraphQL:
  ```bash
  BOT=$(gh api "users/copilot-pull-request-reviewer%5Bbot%5D" -q .node_id)
  PR=$(gh api graphql -f query='{repository(owner:"jethome-iot",name:"esphome-device-configs"){pullRequest(number:<n>){id}}}' -q .data.repository.pullRequest.id)
  gh api graphql -f query='mutation($pr:ID!,$bot:ID!){requestReviews(input:{pullRequestId:$pr,botIds:[$bot],union:true}){clientMutationId}}' -f pr="$PR" -f bot="$BOT"
  ```
  Its comments land asynchronously, usually within a minute or two. Poll
  `gh api repos/jethome-iot/esphome-device-configs/pulls/<n>/reviews` (summaries, authored by
  `copilot-pull-request-reviewer[bot]`) and `.../pulls/<n>/comments` (inline). **Copilot is
  fallible — judge each comment on its merits.** Fix what you agree with, briefly say why for
  what you don't. Finish the triage before asking to merge.
- **Wait for CI, and read what it says.** `.github/workflows/build.yml` matrix-compiles every
  `JXD/*.yaml` and `E1/*.yaml` on the pinned ESPHome. Watch it (`gh pr checks <n> --watch`)
  and act on failures — a red matrix leg is the config CI actually builds, not the one you
  built locally. Note that adding a workflow does not retroactively run it on already-open
  PRs; a stale branch needs a push before Build fires at all.
- **Merge only when the user says so in that turn, and only on green CI.** Opening a PR is
  never implicit permission to merge it. Feature PRs into `master` are squash-merged.

## Comments are terse — and the "why" is not a comment

**One line per comment, two at the most, roughly one per function or block.** Keep the line
that records a real trap — why a call is deliberately absent, which of two similar values a
helper returns, why a pin is inverted. Delete anything that restates the code.

**Explanations of *why a change was made* do not belong in the source at all.** They go in
the commit message, the PR description, or `doc/DECISIONS.md` — that file exists precisely
so that rationale has somewhere better to live than a comment block that rots next to code
nobody edits together with it. Applies to YAML, Python and C++ alike.

Write it terse the first time. A comment trimmed once grows back on the next edit unless
every new line is written to this budget.

## Conventions

- YAML 2-space indent; `.yamllint` and `.pre-commit-config.yaml` are the authority.
- Never read or analyse generated output under `.esphome/build/` — gitignored artifacts.
- Menu items contributed from several packages merge in `packages:` declaration order.
  Upstream `display_menu_base` has no ordering knobs (no `weight:`, no `position:`), so the
  only lever is where the package sits in the entry file's `packages:` list.
- Primary config for verifying a build: `JXD/jxd-r6-e1eth-lcd-eth.yaml`.
