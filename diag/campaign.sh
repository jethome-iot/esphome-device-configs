#!/usr/bin/env bash
# E2 campaign: hammer each HTTP surface in its own run and record the internal-heap delta.
set -u
cd "$(dirname "$0")"
PY=/home/alexey/dev/wt-esphome-device-configs/heap-qemu/.venv/bin/python

run() { echo "############ $* ############"; "$PY" e2.py "$@" 2>&1 | tail -5; }

case "${1:-all}" in
a)
  # Cheap GETs, 2000 requests each: a 50 B/request leak is 100 KB, far above the ~600 B noise.
  for s in ws_page ws_rest ws_detail \
           dash_info dash_status dash_network dash_auth dash_capabilities dash_entities \
           dash_entity_settings dash_entity_settings_meta \
           ae_list ae_entities ae_schema ae_ping ae_export ae_get \
           fb_info fb_list fb_read \
           auth_rejected auth_none origin_rejected notfound; do
    run "$s" -n 2000 --settle 2
  done
  ;;
b)
  # Writes and the big page: fewer requests, they touch LittleFS or move 230 KB each.
  run dash_page -n 400 --settle 2
  for s in fb_download fb_write fb_upload fb_mkdir_delete fb_rename_copy \
           ae_save dash_entity_settings_post dash_auth_post ws_command; do
    run "$s" -n 400 --settle 2
  done
  ;;
c)
  # SSE: does the deficit scale with the number of connect/disconnect cycles or plateau?
  run ws_events -n 100 --settle 3
  run ws_events -n 400 --settle 3
  run ws_events_raw -n 400 --settle 3
  run sse_abandon -n 400 --settle 3
  ;;
d)
  # Idle control, and the same surfaces without keep-alive (one TCP connection per request).
  run idle -n 0 --settle 8 --warmup 0
  run dash_status -n 2000 --settle 2 --no-keepalive
  run dash_info -n 2000 --settle 2 --no-keepalive
  run ws_page -n 2000 --settle 2 --no-keepalive
  run idle -n 0 --settle 8 --warmup 0
  ;;
e)
  # The outbound HTTPS manifest fetch: the biggest internal allocation the firmware makes,
  # and on its own timer it runs every 6 h — 4 to 8 times inside the 24-48 h window of #60.
  run fw_check -n 30 --settle 3 --warmup 2
  ;;
esac
echo "############ campaign $1 done ############"
