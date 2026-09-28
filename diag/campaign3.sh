#!/usr/bin/env bash
# Everything group A did not reach, on the instrumented build: writes, the file browser, the
# auth and origin rejections, the SSE cycles, the unique-URL variants and the idle control.
set -u
cd "$(dirname "$0")"
PY=/home/alexey/dev/wt-esphome-device-configs/heap-qemu/.venv/bin/python
run()  { echo "############ $* ############"; "$PY" -u e2.py  "$@" 2>&1 | tail -5; }
runu() { echo "############ (unique) $* ############"; "$PY" -u e2u.py "$@" 2>&1 | tail -5; }

# Is the 108 B/poll seen at n=20 per poll or a one-off? 60 polls tells them apart.
run fw_check -n 60 --settle 3 --warmup 1

# The idle control, first and last, so a drift between them is visible.
run idle -n 0 --settle 12 --warmup 0

# SSE connect/disconnect: does a deficit scale with the cycle count or plateau?
run ws_events -n 100 --settle 3
run ws_events -n 400 --settle 3
run ws_events_raw -n 400 --settle 3
run sse_abandon -n 400 --settle 3

# The file browser and the rejection paths.
for s in fb_info fb_list fb_read fb_download fb_write fb_upload fb_mkdir_delete fb_rename_copy \
         auth_rejected auth_none origin_rejected notfound ws_command \
         ae_save dash_entity_settings_post dash_auth_post; do
  run "$s" -n 500 --settle 2
done
run dash_page -n 200 --settle 2

# Every request different, so a container keyed by anything the request carries would grow.
for s in notfound_unique fb_read_unique es_unique_type es_unique_source \
         ae_get_unique ws_entity_unique auth_unique_user; do
  runu "$s" -n 1000 --settle 2
done
runu fb_write_unique -n 300 --settle 2

# One TCP connection per request, to separate a per-connection leak from a per-request one.
run dash_status -n 1000 --settle 2 --no-keepalive
run ws_page -n 1000 --settle 2 --no-keepalive

run idle -n 0 --settle 12 --warmup 0
echo "############ campaign3 done ############"
