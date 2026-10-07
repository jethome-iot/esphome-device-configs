# web_server_idf

**This component is temporary.** It is ESPHome's own `web_server_idf` with two fixes the pinned
release lacks, each marked `JetHome:` in the source. Go back to upstream's component on the first
ESPHome bump whose release carries both.

**The httpd task's stack is 8192 bytes**, not upstream's 4352. Every web handler runs on that task,
and the ones that write LittleFS overran 4352 on hardware, panicking the device. See issue #64.

**[esphome/esphome#17800](https://github.com/esphome/esphome/pull/17800), "Close stalled EventSource
sessions via HTTPD"**, is applied ahead of its release: a `/events` client that stops reading no longer
leaves a freed session behind that panics the device later. See issue #70.

How a bump re-applies them, and how to remove the copy, is in
[doc/DEVELOPMENT.md](../../doc/DEVELOPMENT.md), "Bumping ESPHome".

## Checking the behaviour on a device

`events-stall.py` in
[jxd-devices-utils](https://github.com/jethome-iot/jxd-devices-utils) stalls `/events` clients
and reports whether the device closes each stalled session itself. It is written to be run against
both an unfixed and a fixed firmware and to say which it is looking at.
