# web_server_idf

**This component is temporary. Delete it on the ESPHome bump that carries the fix it backports.**

It is a copy of ESPHome's own `web_server_idf` with one upstream commit applied ahead of its release:
[esphome/esphome#17800](https://github.com/esphome/esphome/pull/17800), "Close stalled EventSource
sessions via HTTPD", merged on 2026-09-17. Nothing here is a change of JetHome's own — the `JetHome:`
markers only exist because `scripts/vendored-diff.py --check` requires every divergence from the pinned
release to be marked.

Without it, a `/events` client that stops reading — a dashboard tab on a sleeping phone is enough — had
its session object freed while `esp_http_server` still held the pointer. The object was written to
after release, corrupting whatever took the block over, and the device panicked hours later in an
unrelated subsystem. Each occurrence also lost about 8 KB of internal heap and one of the seven session
slots for good. See issue #70.

## Removing it

The commit is `9309cf96b9`. A release carries it when

```bash
git -C <esphome checkout> merge-base --is-ancestor 9309cf96b9 <tag>
```

succeeds. At that bump, delete this directory,
`devices/JXD/packages/features/web-server-idf-backport.yaml` and that package's line in both device
configs, regenerate `dist/`, and drop the `web_server_idf` entry from
[doc/ARCHITECTURE.md](../../doc/ARCHITECTURE.md). Nothing else depends on it: the shadow exists only
because the package names it in `external_components`.

Run `scripts/vendored-diff.py web_server_idf` first. It prints what is actually being carried, and if
that is no longer #17800 alone, the release has moved this code for some other reason and the diff has
to be read before anything is removed.

## Checking the behaviour on a device

`events-stall.py` in
[jethome-devices-utils](https://github.com/jethome-iot/jethome-devices-utils) stalls `/events` clients
and reports whether the device closes each stalled session itself. It is written to be run against
both an unfixed and a fixed firmware and to say which it is looking at.
