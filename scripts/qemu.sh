#!/usr/bin/env bash
# scripts/qemu.sh — build and run a REAL JXD device config under Espressif QEMU.
#
# There is no separate "qemu device" config to keep in sync: the real device
# YAML is included verbatim as a package and the QEMU overlay
# (include/features/qemu.yaml, plus an optional per-device one) is layered on
# top of it. Later packages win, so the overlays only override what QEMU cannot
# emulate: the Ethernet PHY, the I2C bus and everything behind it, the ADC, the
# flash mode and the front panel. See doc/QEMU.md.
#
# The generated wrapper lands in JXD/ next to the device configs (`../fonts/*.bdf`
# and `../components` resolve relative to the main config's directory) and is
# named <device>.qemu.yaml — gitignored, regenerated on every run.
#
# Usage:
#   scripts/qemu.sh run <device> [options]     build (unless --no-build), then boot in QEMU
#   scripts/qemu.sh build <device> [options]   compile the QEMU flavour only
#   scripts/qemu.sh image <device> [options]   (re)create the padded flash image only
#   scripts/qemu.sh list                       device configs, marking the running ones
#   scripts/qemu.sh stop [<device>]            stop instances, keep flash state and build
#   scripts/qemu.sh clean [<device>]           stop instances, drop wrappers and images
#   scripts/qemu.sh help
#
# An emulator left running holds its forwarded ports and its flash image, so stop
# it when the check it was started for is over: `stop` to keep the emulated flash
# (saved config, LittleFS) for next time, `clean` to throw it away too.
#
# Options:
#   --http-port <p>    host port forwarded to the device's web server  (default 8080)
#   --api-port <p>     host port forwarded to the native API 6053      (default 6053)
#   --ota-port <p>     host port forwarded to the OTA listener 3232    (default 3232)
#   --psram <size>     PSRAM given to the machine: 2M|4M|none          (default 4M)
#   --fresh            recreate the flash image, wiping emulated flash state
#                      (NVS, LittleFS, saved config) — like erasing a real device
#   --no-build         skip `esphome compile`, use whatever was built last
#   --no-wdt           disable the emulated Timer Group watchdogs
#   --daemon           run QEMU in the background; log to the build dir
#   --wait-http <sec>  with --daemon: poll the web server until it answers,
#                      exit non-zero if it does not come up in time
#
# Prerequisite: Espressif's QEMU fork (upstream qemu-system-xtensa has no `esp32`
# machine). Install with:
#   IDF_PATH=~/.platformio/packages/framework-espidf \
#     python3 ~/.platformio/packages/framework-espidf/tools/idf_tools.py install qemu-xtensa
# Its user-mode networking needs libslirp: `sudo apt install libslirp0`.
# Point QEMU_XTENSA at the binary to override discovery.
set -Eeuo pipefail

die()  { printf 'qemu: %s\n' "$*" >&2; exit 1; }
info() { printf '%s\n' "$*"; }

usage() { sed -n '2,/^set /p' "${BASH_SOURCE[0]}" | sed '/^set /d; s/^# \{0,1\}//'; }

SELF_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd -- "$SELF_DIR/.." && pwd)
CONFIG_DIR="$ROOT/JXD"
OVERLAY="../include/features/qemu.yaml"

[ -d "$CONFIG_DIR" ] || die "device configs not found at $CONFIG_DIR"

# --- options -----------------------------------------------------------------
HTTP_PORT=8080
API_PORT=6053
OTA_PORT=3232
PSRAM=4M
FRESH=0
DO_BUILD=1
NO_WDT=0
DAEMON=0
WAIT_HTTP=0

CMD=${1:-help}
[ $# -gt 0 ] && shift || true
DEVICE=""

# The accepted value goes into OPT_VALUE rather than to stdout: `die` inside a
# command substitution only kills the subshell, so the caller went on with an
# empty value and every later check added its own message. `--http-port` as the
# last argument would also abort on an unbound $2 with a raw bash error.
OPT_VALUE=""
opt_arg() {
  [ -n "${2:-}" ] || die "$1 needs a value"
  OPT_VALUE=$2
}
opt_uint() {
  opt_arg "$@"
  case "$OPT_VALUE" in ''|*[!0-9]*) die "$1 must be a whole number, got '$OPT_VALUE'" ;; esac
}
opt_port() {
  opt_uint "$@"
  { [ "$OPT_VALUE" -ge 1 ] && [ "$OPT_VALUE" -le 65535 ]; } ||
    die "$1 must be a TCP port (1-65535), got '$OPT_VALUE'"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --http-port) opt_port "$1" "${2:-}"; HTTP_PORT=$OPT_VALUE; shift 2 ;;
    --api-port)  opt_port "$1" "${2:-}"; API_PORT=$OPT_VALUE;  shift 2 ;;
    --ota-port)  opt_port "$1" "${2:-}"; OTA_PORT=$OPT_VALUE;  shift 2 ;;
    --wait-http) opt_uint "$1" "${2:-}"; WAIT_HTTP=$OPT_VALUE; shift 2 ;;
    --psram)     opt_arg  "$1" "${2:-}"; PSRAM=$OPT_VALUE;     shift 2 ;;
    --fresh)     FRESH=1;      shift ;;
    --no-build)  DO_BUILD=0;   shift ;;
    --no-wdt)    NO_WDT=1;     shift ;;
    --daemon)    DAEMON=1;     shift ;;
    -h|--help)   usage; exit 0 ;;
    -*)          die "unknown option: $1" ;;
    *)           [ -n "$DEVICE" ] && die "unexpected argument: $1"; DEVICE=$1; shift ;;
  esac
done
case "$PSRAM" in 2M|4M|none) ;; *) die "--psram must be 2M, 4M or none, got '$PSRAM'" ;; esac

# --- helpers -----------------------------------------------------------------

# Accepts a bare device name or a path to the device YAML; echoes the bare name.
resolve_device() {
  local d=${1:-}
  [ -n "$d" ] || die "no device given (see: scripts/qemu.sh list)"
  d=$(basename -- "$d"); d=${d%.yaml}; d=${d%.qemu}
  [ -f "$CONFIG_DIR/$d.yaml" ] || die "no such device config: JXD/$d.yaml"
  # The radio is not emulated and the -wifi config starts its AP from on_boot.
  case "$d" in *-wifi) die "$d cannot run under QEMU — Wi-Fi is not emulated; use the -eth config" ;; esac
  printf '%s' "$d"
}

list_devices() {
  local f
  for f in "$CONFIG_DIR"/*.yaml; do
    [ -e "$f" ] || continue
    case "$f" in *.qemu.yaml) continue ;; esac
    basename -- "${f%.yaml}"
  done
}

# Ordered best-first: an explicit override, then the newest Espressif install,
# then whatever is on PATH — which on many machines is the distro build, and that
# one has no `esp32` machine at all. A candidate that is upstream, or that cannot
# start, is skipped rather than fatal, so one bad entry never hides a good one.
find_qemu() {
  local candidates=() installs=() c
  # `sort -V` gives oldest first; prepend so the newest install ends up first.
  while IFS= read -r c; do installs=("$c" ${installs[@]+"${installs[@]}"}); done < <(
    ls -d "$HOME"/.espressif/tools/qemu-xtensa/*/qemu/bin/qemu-system-xtensa 2>/dev/null | sort -V
  )
  [ -n "${QEMU_XTENSA:-}" ] && candidates+=("$QEMU_XTENSA")
  [ ${#installs[@]} -gt 0 ] && candidates+=("${installs[@]}")
  command -v qemu-system-xtensa >/dev/null 2>&1 && candidates+=("$(command -v qemu-system-xtensa)")
  [ ${#candidates[@]} -gt 0 ] || die "qemu-system-xtensa not found — see the install hint in $0"

  local qemu libdir out slirp_missing=0 upstream_seen=0
  for qemu in "${candidates[@]}"; do
    [ -x "$qemu" ] || continue
    # The published tarballs do not carry libslirp, but an install may have had
    # it vendored next to the binary; the arch subdirectory differs, so take
    # whatever is there and fall back to the system one.
    libdir=$(dirname -- "$qemu")/../lib
    libdir=$(cd -- "$libdir" 2>/dev/null && ls -d "$PWD"/*-linux-gnu 2>/dev/null | head -1) || libdir=""
    if out=$(LD_LIBRARY_PATH="${libdir}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" "$qemu" --version 2>&1); then
      case "$out" in
        *esp_develop*) QEMU_BIN=$qemu; QEMU_LIBDIR=$libdir; return 0 ;;
        *) upstream_seen=1; continue ;;
      esac
    fi
    case "$out" in *libslirp*) slirp_missing=1 ;; esac
  done
  [ "$slirp_missing" -eq 1 ] && die "qemu-system-xtensa needs libslirp — install it with: sudo apt install libslirp0"
  [ "$upstream_seen" -eq 1 ] && die "only upstream qemu-system-xtensa found (no \`esp32\` machine) — install Espressif's fork, see the hint in $0"
  die "found qemu-system-xtensa but could not run it"
}

activate_env() {
  # shellcheck disable=SC1091
  [ -f "$ROOT/.venv/bin/activate" ] && . "$ROOT/.venv/bin/activate"
  command -v esphome >/dev/null 2>&1 || die "esphome not on PATH — activate the venv first"
}

# Writes <device>.qemu.yaml: the real config plus the QEMU overlay, renamed so
# the emulator build never fights the hardware build over one build directory.
write_wrapper() {
  local device=$1 wrapper="$CONFIG_DIR/$1.qemu.yaml" extra="../include/features/qemu-$1.yaml"
  {
    printf '# GENERATED by scripts/qemu.sh — do not edit, do not commit.\n'
    printf '# The real device config, layered with the QEMU overlay.\n\n'
    printf 'substitutions:\n  name: %s-qemu\n\n' "$device"
    printf 'packages:\n'
    printf '  device: !include %s.yaml\n' "$device"
    printf '  qemu: !include %s\n' "$OVERLAY"
    [ -f "$CONFIG_DIR/$extra" ] && printf '  qemu_device: !include %s\n' "$extra"
  } > "$wrapper"
  printf '%s' "$wrapper"
}

build_dir()   { printf '%s/.esphome/build/%s-qemu' "$CONFIG_DIR" "$1"; }
factory_bin() { printf '%s/build/firmware.factory.bin' "$(build_dir "$1")"; }
sdkconfig()   { printf '%s/sdkconfig.%s-qemu' "$(build_dir "$1")" "$1"; }
flash_image() { printf '%s/qemu-flash.bin' "$(build_dir "$1")"; }

do_build() {
  local device=$1 wrapper
  wrapper=$(write_wrapper "$device")
  info "==> compiling $device (QEMU flavour)"
  esphome compile "$wrapper"
}

# QEMU wants the flash image to be exactly the size the bootloader header
# advertises, and the gaps filled with erased flash (0xFF) — esptool pads with
# 0xFF, a plain truncate would write zeros and leave NVS/LittleFS confused.
do_image() {
  local device=$1 factory image config flash_size
  factory=$(factory_bin "$device")
  image=$(flash_image "$device")
  config=$(sdkconfig "$device")
  [ -f "$factory" ] || die "no firmware built yet for $device — run without --no-build"
  if [ -f "$image" ] && [ "$FRESH" -eq 0 ] && [ "$image" -nt "$factory" ]; then
    return 0
  fi
  # Without this, `set -e` kills the script on the sed below with a raw `sed:` message.
  [ -f "$config" ] || die "no sdkconfig for $device at $config — run without --no-build"
  # No `| head -1`: sed would take SIGPIPE on a second match and pipefail would
  # turn that into a silent exit rather than the message below.
  flash_size=$(sed -n '/^CONFIG_ESPTOOLPY_FLASHSIZE="\(.*\)"$/{s//\1/p;q;}' "$config")
  [ -n "$flash_size" ] || die "could not read CONFIG_ESPTOOLPY_FLASHSIZE from $config"
  info "==> flash image: $flash_size (padded with 0xFF)"
  python -m esptool --chip esp32 merge-bin \
    --flash-size "$flash_size" --pad-to-size "$flash_size" \
    --output "$image" 0x0 "$factory" >/dev/null
}

# Instances are identified by the flash image on their command line, which is
# unique per device. The pidfile is only a convenience for the user: an
# interactive session `exec`s and never cleans up, a killed one leaves it behind,
# and a live instance can end up with no pidfile at all — so trusting it would
# both miss running instances and, after PID reuse, signal something unrelated.
qemu_pids_for() {
  local image=$1 pid cmdline
  # Matched on `if=mtd` and not on the binary name: $QEMU_XTENSA may point at a
  # binary called anything, and `pgrep -x` cannot see this one either — procps
  # refuses a pattern longer than the 15-char comm. The exact `file=$image,`
  # below is what narrows it to this device, and to this checkout.
  for pid in $(pgrep -f 'if=mtd' 2>/dev/null || true); do
    # 2>/dev/null goes first: redirections are applied left to right, and it is
    # the *input* one that fails noisily when the process exits mid-scan.
    cmdline=$(tr '\0' ' ' 2>/dev/null < "/proc/$pid/cmdline") || continue
    case "$cmdline" in *"file=$image,"*) printf '%s\n' "$pid" ;; esac
  done
}

# A leftover instance keeps the forwarded ports *and* its flash image open: the
# new QEMU dies on "Could not set up host forwarding rule", and the old one's
# writeback can clobber a freshly rebuilt image. Never leave one behind.
stop_previous() {
  local device=$1 image pid waited
  image=$(flash_image "$device")
  for pid in $(qemu_pids_for "$image"); do
    info "==> stopping previous QEMU (pid $pid)"
    kill "$pid" 2>/dev/null || true
    waited=0
    while kill -0 "$pid" 2>/dev/null && [ "$waited" -lt 10 ]; do sleep 1; waited=$((waited + 1)); done
    kill -9 "$pid" 2>/dev/null || true
  done
  rm -f "$(build_dir "$device")/qemu.pid"
}

# QEMU refuses to start at all if one forwarded port is taken — usually another
# emulated device still running — and says so only in its log. Name the port here
# instead, where the caller is looking.
check_ports_free() {
  local port
  # Two of ours pointed at the same host port is the one clash `ss` cannot see —
  # nothing is listening yet, and QEMU still refuses. `--http-port 6053` with the
  # API left at its default is the realistic way to hit it.
  if [ "$(printf '%s\n' "$HTTP_PORT" "$API_PORT" "$OTA_PORT" | sort -u | wc -l)" -ne 3 ]; then
    die "--http-port/--api-port/--ota-port must differ, got $HTTP_PORT/$API_PORT/$OTA_PORT"
  fi
  for port in "$HTTP_PORT" "$API_PORT" "$OTA_PORT"; do
    if ss -ltn "sport = :$port" 2>/dev/null | grep -q LISTEN; then
      die "port $port is already in use — another device is likely running (see: scripts/qemu.sh list), or pass --http-port/--api-port/--ota-port"
    fi
  done
}

# QEMU diagnoses a refused start ("Could not set up host forwarding rule") only in
# its log, so hand the caller the end of it rather than just a path.
check_alive() {
  local pid=$1 logfile=$2
  if ! kill -0 "$pid" 2>/dev/null; then
    [ -s "$logfile" ] && tail -n 10 -- "$logfile" >&2
    die "QEMU exited early — see $logfile"
  fi
}

do_run() {
  local device=$1 image logfile args
  image=$(flash_image "$device")
  logfile="$(build_dir "$device")/qemu.log"
  # Again, deliberately: `run` checks up front to fail before compiling, but a
  # build takes minutes and someone else can take the port in the meantime.
  check_ports_free
  [ -n "$QEMU_LIBDIR" ] && export LD_LIBRARY_PATH="$QEMU_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

  # Bound to the loopback address explicitly: an empty hostaddr means INADDR_ANY,
  # which would put an unauthenticated device web server on every interface of
  # the machine.
  args=(-nographic -machine esp32
        -drive "file=$image,if=mtd,format=raw"
        -nic "user,model=open_eth,hostfwd=tcp:127.0.0.1:$HTTP_PORT-:80,hostfwd=tcp:127.0.0.1:$API_PORT-:6053,hostfwd=tcp:127.0.0.1:$OTA_PORT-:3232")
  [ "$PSRAM" != "none" ] && args+=(-m "$PSRAM")
  [ "$NO_WDT" -eq 1 ] && args+=(-global driver=timer.esp32.timg,property=wdt_disable,value=true)

  info "==> $device on QEMU"
  info "    web    http://127.0.0.1:$HTTP_PORT"
  info "    api    127.0.0.1:$API_PORT   (esphome logs <config> --device 127.0.0.1)"
  info "    flash  $image"
  if [ "$DAEMON" -eq 1 ]; then
    nohup "$QEMU_BIN" "${args[@]}" >"$logfile" 2>&1 &
    local pid=$!
    echo "$pid" > "$(build_dir "$device")/qemu.pid"
    info "    pid    $pid   (log: $logfile)"
    # A port taken since check_ports_free kills QEMU within milliseconds, and it
    # says so only in the log — exiting 0 here would tell CI a dead emulator ran.
    sleep 1
    check_alive "$pid" "$logfile"
    if [ "$WAIT_HTTP" -gt 0 ]; then
      local waited=0
      while [ "$waited" -lt "$WAIT_HTTP" ]; do
        check_alive "$pid" "$logfile"
        # No -f: "up" is any answer at all, including an error status.
        if curl -sS -o /dev/null --max-time 2 "http://127.0.0.1:$HTTP_PORT/" 2>/dev/null; then
          info "==> web server answered after ${waited}s"
          return 0
        fi
        sleep 2; waited=$((waited + 2))
      done
      die "web server did not come up within ${WAIT_HTTP}s — see $logfile"
    fi
  else
    info "    exit   Ctrl-A x"
    # exec keeps this PID, so recording it now lets a later run clean up after a
    # session that was killed rather than exited.
    echo "$$" > "$(build_dir "$device")/qemu.pid"
    exec "$QEMU_BIN" "${args[@]}"
  fi
}

# The counterpart to `run`: same teardown `run` and `clean` do, on its own and
# without touching the build. Silence would be indistinguishable from a device
# that was never named right, so say when nothing was running.
do_stop() {
  local device=${1:-} d stopped=0 devices=()
  if [ -n "$device" ]; then
    devices=("$device")
  else
    mapfile -t devices < <(list_devices)
  fi
  for d in ${devices[@]+"${devices[@]}"}; do
    [ -n "$(qemu_pids_for "$(flash_image "$d")")" ] && stopped=$((stopped + 1))
    # Unconditionally, like `clean`: a session that was killed rather than exited
    # leaves a pidfile with no process, and that is exactly what `stop` is for.
    stop_previous "$d"
  done
  [ "$stopped" -gt 0 ] || info "no emulator running${device:+ for $device}"
}

# What `check_ports_free` sends the caller to when a port is taken, so it has to
# name every forwarded port, not just the web one — the clash is as often on the
# API or OTA port, and they are recorded nowhere but the running command line.
hostfwd_port() {
  printf '%s' "$1" |
    sed -n "s/.*hostfwd=tcp:127\.0\.0\.1:\([0-9]\{1,\}\)-:$2[^0-9].*/\1/p"
}

do_list() {
  local d pid pids cmdline http api ota devices=()
  mapfile -t devices < <(list_devices)
  for d in ${devices[@]+"${devices[@]}"}; do
    # `| head -1` would be a SIGPIPE away from killing the whole listing under
    # pipefail the moment a device has two instances — which is when you are
    # looking at `list` in the first place.
    pids=$(qemu_pids_for "$(flash_image "$d")")
    pid=${pids%%$'\n'*}
    if [ -z "$pid" ]; then
      info "$d"
      continue
    fi
    cmdline=$(tr '\0' ' ' 2>/dev/null < "/proc/$pid/cmdline" || true)
    http=$(hostfwd_port "$cmdline" 80)
    api=$(hostfwd_port "$cmdline" 6053)
    ota=$(hostfwd_port "$cmdline" 3232)
    info "$d — running (pid $pid)${http:+  web http://127.0.0.1:$http}${api:+  api $api}${ota:+  ota $ota}"
  done
}

# Deletes the flash image out from under a running instance otherwise, leaving a
# QEMU nobody can find holding the ports.
do_clean() {
  local device=${1:-} d devices=()
  if [ -n "$device" ]; then
    stop_previous "$device"
    rm -f "$CONFIG_DIR/$device.qemu.yaml" "$(flash_image "$device")" \
          "$(build_dir "$device")/qemu.pid" "$(build_dir "$device")/qemu.log"
    info "cleaned $device"
  else
    mapfile -t devices < <(list_devices)
    for d in ${devices[@]+"${devices[@]}"}; do stop_previous "$d"; done
    rm -f "$CONFIG_DIR"/*.qemu.yaml
    rm -f "$CONFIG_DIR"/.esphome/build/*-qemu/qemu-flash.bin \
          "$CONFIG_DIR"/.esphome/build/*-qemu/qemu.pid \
          "$CONFIG_DIR"/.esphome/build/*-qemu/qemu.log
    info "cleaned generated wrappers and flash images"
  fi
}

case "$CMD" in
  list)  do_list ;;
  stop)
    [ -n "${DEVICE:-}" ] && DEVICE=$(resolve_device "$DEVICE")
    do_stop "${DEVICE:-}" ;;
  clean)
    # Through resolve_device so a stray path cannot send rm outside the config dir
    [ -n "${DEVICE:-}" ] && DEVICE=$(resolve_device "$DEVICE")
    do_clean "${DEVICE:-}" ;;
  build)
    DEVICE=$(resolve_device "$DEVICE"); activate_env; do_build "$DEVICE" ;;
  image)
    DEVICE=$(resolve_device "$DEVICE"); activate_env; do_image "$DEVICE" ;;
  run)
    DEVICE=$(resolve_device "$DEVICE"); activate_env
    # Everything that can refuse to proceed runs before anything is touched, and
    # the old instance goes before the image it holds is rewritten.
    find_qemu
    stop_previous "$DEVICE"
    # After stop_previous, so re-running the same device is not a clash with
    # itself, and before the build, so a taken port costs a message and not a
    # full compile.
    check_ports_free
    [ "$DO_BUILD" -eq 1 ] && do_build "$DEVICE"
    do_image "$DEVICE"
    do_run "$DEVICE" ;;
  help|-h|--help) usage ;;
  *) die "unknown command: $CMD (try: scripts/qemu.sh help)" ;;
esac
