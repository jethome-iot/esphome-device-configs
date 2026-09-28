#!/usr/bin/env bash
# E2 campaign, part two. Kept separate from campaign.sh so the running script is never edited.
set -u
cd "$(dirname "$0")"
PY=/home/alexey/dev/wt-esphome-device-configs/heap-qemu/.venv/bin/python

run()  { echo "############ $* ############"; "$PY" e2.py  "$@" 2>&1 | tail -5; }
runu() { echo "############ (unique) $* ############"; "$PY" e2u.py "$@" 2>&1 | tail -5; }

case "${1:-}" in
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
  # Idle control, and the same surfaces with one TCP connection per request.
  run idle -n 0 --settle 10 --warmup 0
  run dash_status -n 2000 --settle 2 --no-keepalive
  run dash_info -n 2000 --settle 2 --no-keepalive
  run ws_page -n 2000 --settle 2 --no-keepalive
  run idle -n 0 --settle 10 --warmup 0
  ;;
e)
  # The outbound HTTPS manifest fetch: the biggest internal allocation the firmware makes,
  # and on its own timer it runs every 6 h — 4 to 8 times inside the 24-48 h window of #60.
  run fw_check -n 30 --settle 3 --warmup 2
  ;;
f)
  # Every request different, so a container keyed by anything the request carries would grow.
  for s in notfound_unique fb_read_unique es_unique_type es_unique_source \
           ae_get_unique ws_entity_unique auth_unique_user; do
    runu "$s" -n 2000 --settle 2
  done
  runu fb_write_unique -n 400 --settle 2
  ;;
*)
  echo "usage: campaign2.sh b|c|d|e|f" >&2
  exit 2
  ;;
esac
echo "############ campaign2 $1 done ############"
