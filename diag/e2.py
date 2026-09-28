#!/usr/bin/env python3
"""E2: hammer one HTTP surface of the emulated JXD and report the internal-heap delta.

Heap samples come from the HEAPCSV lines the firmware logs on the UART (qemu.log), so
sampling itself costs no HTTP request and a "no traffic" control really is one.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import socket
import sys
import time
from dataclasses import dataclass

import requests
from requests.adapters import HTTPAdapter
from requests.auth import HTTPDigestAuth

BASE = os.environ.get("E2_BASE", "http://127.0.0.1:8091")
QEMU_LOG = os.environ.get(
    "E2_LOG",
    "/home/alexey/dev/wt-esphome-device-configs/heap-qemu/devices/JXD/"
    ".esphome/build/jxd-r6-e1eth-lcd-qemu/qemu.log",
)
AUTH = HTTPDigestAuth("admin", "admin")
HEAPCSV = re.compile(rb"HEAPCSV," + rb",".join([rb"(\d+)"] * 11))
FIELDS = ("ms", "free", "block", "minfree", "alloc_b", "alloc_n", "free_n", "psram",
          "httpd_hw", "loop_hw", "tasks")


@dataclass
class Sample:
    ms: int
    free: int
    block: int
    minfree: int
    alloc_b: int
    alloc_n: int
    free_n: int
    psram: int
    httpd_hw: int
    loop_hw: int
    tasks: int

    def __str__(self) -> str:
        return (
            f"free={self.free} block={self.block} minfree={self.minfree} "
            f"alloc={self.alloc_b}B/{self.alloc_n}blk free_blk={self.free_n} psram={self.psram} "
            f"httpd_stack_left={self.httpd_hw} loop_stack_left={self.loop_hw} tasks={self.tasks}"
        )


def samples() -> list[Sample]:
    with open(QEMU_LOG, "rb") as fh:
        fh.seek(0, os.SEEK_END)
        size = fh.tell()
        fh.seek(max(0, size - 400_000))
        data = fh.read()
    return [Sample(*(int(g) for g in m.groups())) for m in HEAPCSV.finditer(data)]


def last_sample() -> Sample:
    got = samples()
    if not got:
        sys.exit("no HEAPCSV lines in the QEMU log yet")
    return got[-1]


def wait_samples(n: int, label: str) -> Sample:
    """Settle: wait for n fresh HEAPCSV lines, then take the last."""
    start = len(samples())
    deadline = time.time() + 20 * n + 60
    while len(samples()) < start + n:
        if time.time() > deadline:
            sys.exit(f"the firmware stopped logging HEAPCSV during {label}")
        time.sleep(1)
    return last_sample()


# --- the surfaces -----------------------------------------------------------------


def _get(s: requests.Session, path: str, **kw):
    return s.get(BASE + path, timeout=30, **kw)


def _post(s: requests.Session, path: str, **kw):
    kw.setdefault("data", "")
    return s.post(BASE + path, timeout=30, **kw)


def ws_rest(s, i):
    _get(s, "/sensor/PCB%20Temp")
    _get(s, "/switch/Relay%201")


def ws_page(s, i):
    # web_server's own index is never served (the dashboard owns /), so this is the
    # closest stock-web_server page: the entity list a REST client walks.
    _get(s, "/text_sensor/ResetReason")


def ws_command(s, i):
    _post(s, "/switch/Relay%201/toggle")


def ws_events(s, i):
    """Connect to the SSE stream, read one event, disconnect."""
    r = _get(s, "/events", stream=True)
    try:
        for _ in r.iter_lines():
            break
    finally:
        r.close()


def ws_events_raw(s, i):
    """Same, but a bare socket closed without a graceful HTTP shutdown."""
    sock = socket.create_connection(("127.0.0.1", int(BASE.rsplit(":", 1)[1])), timeout=10)
    sock.sendall(b"GET /events HTTP/1.1\r\nHost: x\r\n\r\n")
    sock.recv(256)
    sock.close()


class _SmallRcvAdapter(HTTPAdapter):
    """A 2 KB receive buffer, so a client that stops reading closes the device's send
    window within a few events instead of after a megabyte."""

    def init_poolmanager(self, *args, **kwargs):
        kwargs["socket_options"] = [(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)]
        super().init_poolmanager(*args, **kwargs)


_STUCK_HOLD_S = float(os.environ.get("E2_STUCK_HOLD", "30"))


def sse_stuck(s, i):
    """Open /events, never read the body, hold the connection past
    MAX_CONSECUTIVE_SEND_FAILURES, then close. This is the path
    web_server_idf.cpp:899-907 logs as "Closing stuck EventSource connection"."""
    stuck = requests.Session()
    stuck.auth = AUTH
    stuck.mount("http://", _SmallRcvAdapter())
    r = stuck.get(BASE + "/events", stream=True, timeout=30)
    time.sleep(_STUCK_HOLD_S)
    r.close()
    stuck.close()


def sse_abandon(s, i):
    """Open /events with a tiny receive buffer and drop the socket with SO_LINGER 0, so
    the device sees a RST rather than a clean FIN."""
    port = int(BASE.rsplit(":", 1)[1])
    sock = socket.create_connection(("127.0.0.1", port), timeout=10)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, __import__("struct").pack("ii", 1, 0))
    sock.sendall(b"GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
    sock.recv(64)
    sock.close()


def dash_page(s, i):
    _get(s, "/")


def dash_info(s, i):
    _get(s, "/api/device/info")


def dash_status(s, i):
    _get(s, "/api/device/status")


def dash_network(s, i):
    _get(s, "/api/device/network")


def dash_auth(s, i):
    _get(s, "/api/device/auth")


def dash_capabilities(s, i):
    _get(s, "/api/device/capabilities")


def dash_entities(s, i):
    _get(s, "/api/device/entities")


def dash_entity_settings(s, i):
    _get(s, "/api/device/entity-settings?type=switch")


def dash_entity_settings_meta(s, i):
    _get(s, "/api/device/entity-settings-meta")


def dash_entity_settings_post(s, i):
    body = {
        "type": "switch",
        "source_name": "relay_1",
        "settings": {"restore_mode": "RESTORE_DEFAULT_OFF"},
    }
    s.post(
        BASE + "/api/device/entity-settings",
        data=json.dumps(body),
        headers={"Content-Type": "application/json"},
        timeout=30,
    )


def dash_auth_post(s, i):
    s.post(
        BASE + "/api/device/auth",
        data=json.dumps({"username": "admin", "password": "admin"}),
        headers={"Content-Type": "application/json"},
        timeout=30,
    )


def fb_info(s, i):
    _get(s, "/files/info")


def fb_list(s, i):
    _get(s, "/files/list?path=/")


def fb_read(s, i):
    _get(s, "/files/read?path=/e2probe.txt")


def fb_download(s, i):
    _get(s, "/files/download?path=/e2probe.txt")


def fb_write(s, i):
    s.post(
        BASE + "/files/write?path=/e2probe.txt",
        data=b"x" * 256,
        headers={"Content-Type": "text/plain"},
        timeout=30,
    )


def fb_upload(s, i):
    s.post(
        BASE + "/files/upload?path=/e2up.bin",
        files={"file": ("e2up.bin", b"y" * 256)},
        timeout=60,
    )


def fb_mkdir_delete(s, i):
    _post(s, "/files/mkdir", data={"path": f"/e2dir{i % 4}"})
    _post(s, "/files/delete", data={"path": f"/e2dir{i % 4}"})


def ae_list(s, i):
    _get(s, "/automation-editor/api/list")


def ae_entities(s, i):
    _get(s, "/automation-editor/api/entities")


def ae_schema(s, i):
    _get(s, "/automation-editor/api/schema")


def ae_ping(s, i):
    _get(s, "/automation-editor/api/ping")


def ae_export(s, i):
    _get(s, "/automation-editor/api/export")


# save takes ONE rule. id 0/absent creates, an existing id replaces — so the probe rule is
# created once and then overwritten, instead of filling LittleFS with copies.
AE_RULE = {
    "name": "e2probe",
    "enabled": False,
    "mode": "single",
    "triggers": [{"source": "input", "type": "click", "object_id": "input_1"}],
    "actions": [{"source": "switch", "type": "turn_on", "object_id": "red_led"}],
}
_AE_ID = {"id": 0}


def _ae_ensure_rule(s) -> int:
    if _AE_ID["id"]:
        return _AE_ID["id"]
    for name in (None, AE_RULE["name"]):
        if name is None:
            s.post(
                BASE + "/automation-editor/api/save",
                data=json.dumps(AE_RULE),
                headers={"Content-Type": "application/json"},
                timeout=30,
            )
        rows = _get(s, "/automation-editor/api/list").json().get("automations", [])
        for row in rows:
            if row.get("name") == AE_RULE["name"]:
                _AE_ID["id"] = int(row["id"])
                return _AE_ID["id"]
    return 0


def ae_save(s, i):
    rule = dict(AE_RULE, id=_ae_ensure_rule(s))
    s.post(
        BASE + "/automation-editor/api/save",
        data=json.dumps(rule),
        headers={"Content-Type": "application/json"},
        timeout=30,
    )


def ae_get(s, i):
    _get(s, f"/automation-editor/api/get?id={_ae_ensure_rule(s)}")


def auth_rejected(s, i):
    # A separate session: no cached nonce, so this is a real 401 round trip.
    requests.get(BASE + "/api/device/info", auth=HTTPDigestAuth("admin", "wrong"), timeout=30)


def auth_none(s, i):
    requests.get(BASE + "/api/device/info", timeout=30)


def origin_rejected(s, i):
    _get(s, "/api/device/info", headers={"Origin": "http://evil.example"})


def ws_detail(s, i):
    _get(s, "/sensor/HeapFree?detail=all")


def panel_frame(s, i):
    _get(s, "/panel/frame")


def fb_rename_copy(s, i):
    _post(s, "/files/copy", data={"old_path": "/e2probe.txt", "new_path": "/e2copy.txt"})
    _post(s, "/files/rename", data={"old_path": "/e2copy.txt", "new_path": "/e2copy2.txt"})
    _post(s, "/files/delete", data={"path": "/e2copy2.txt"})


def fw_check(s, i):
    """A full outbound HTTPS manifest fetch from fw.jethome.com — what the device does on
    its own every 6 h (firmware_update->set_update_interval(21600000)). The mbedTLS
    handshake is the largest internal allocation this firmware ever makes."""
    _post(s, "/button/Check%20for%20updates/press")
    time.sleep(4)


def notfound(s, i):
    _get(s, "/api/device/no-such-route")


def idle(s, i):
    time.sleep(1)


SURFACES = {
    "ws_rest": ws_rest,
    "ws_page": ws_page,
    "ws_command": ws_command,
    "ws_events": ws_events,
    "ws_events_raw": ws_events_raw,
    "ws_detail": ws_detail,
    "panel_frame": panel_frame,
    "sse_stuck": sse_stuck,
    "sse_abandon": sse_abandon,
    "dash_page": dash_page,
    "dash_info": dash_info,
    "dash_status": dash_status,
    "dash_network": dash_network,
    "dash_auth": dash_auth,
    "dash_capabilities": dash_capabilities,
    "dash_entities": dash_entities,
    "dash_entity_settings": dash_entity_settings,
    "dash_entity_settings_meta": dash_entity_settings_meta,
    "dash_entity_settings_post": dash_entity_settings_post,
    "dash_auth_post": dash_auth_post,
    "fb_info": fb_info,
    "fb_list": fb_list,
    "fb_read": fb_read,
    "fb_download": fb_download,
    "fb_write": fb_write,
    "fb_upload": fb_upload,
    "fb_mkdir_delete": fb_mkdir_delete,
    "fb_rename_copy": fb_rename_copy,
    "ae_list": ae_list,
    "ae_entities": ae_entities,
    "ae_schema": ae_schema,
    "ae_ping": ae_ping,
    "ae_export": ae_export,
    "ae_save": ae_save,
    "ae_get": ae_get,
    "auth_rejected": auth_rejected,
    "auth_none": auth_none,
    "origin_rejected": origin_rejected,
    "notfound": notfound,
    "fw_check": fw_check,
    "idle": idle,
}


def run(name: str, n: int, settle: int, keepalive: bool, warmup: int) -> None:
    fn = SURFACES[name]
    mk = (lambda: _session()) if keepalive else None

    def _session() -> requests.Session:
        s = requests.Session()
        s.auth = AUTH
        return s

    session = _session()
    for i in range(warmup):
        try:
            fn(session, i)
        except Exception as exc:  # noqa: BLE001
            print(f"warmup {i}: {exc}", file=sys.stderr)
    before = wait_samples(settle, f"{name} baseline")
    print(f"[{name}] baseline  {before}", flush=True)

    t0 = time.time()
    errors = 0
    for i in range(n):
        if not keepalive:
            session.close()
            session = _session()
        try:
            fn(session, i)
        except Exception:  # noqa: BLE001
            errors += 1
            if errors > n // 4 + 5:
                print(f"[{name}] too many errors, aborting at {i}", file=sys.stderr)
                break
    dt = time.time() - t0
    session.close()

    window = [s_ for s_ in samples() if s_.ms >= before.ms]
    after = wait_samples(settle, f"{name} after")
    window += [s_ for s_ in samples() if s_.ms > before.ms]
    worst_httpd = min((s_.httpd_hw for s_ in window if s_.httpd_hw), default=0)
    print(f"[{name}] worst httpd stack headroom during the run: {worst_httpd} B")
    print(f"[{name}] after     {after}", flush=True)
    d_free = before.free - after.free
    d_block = before.block - after.block
    d_blocks = after.alloc_n - before.alloc_n
    d_alloc = after.alloc_b - before.alloc_b
    print(
        f"[{name}] n={n} keepalive={keepalive} errors={errors} {dt:.1f}s "
        f"({n / max(dt, 1e-6):.1f} req/s)"
    )
    print(
        f"[{name}] lost_free={d_free}B  ({d_free / max(n, 1):.1f} B/req)  "
        f"lost_block={d_block}B  alloc_blocks+={d_blocks} ({d_blocks / max(n, 1):.3f}/req)  "
        f"alloc_bytes+={d_alloc} ({d_alloc / max(n, 1):.1f} B/req)"
    )
    row = {
        "surface": name,
        "n": n,
        "keepalive": keepalive,
        "seconds": round(dt, 1),
        "errors": errors,
        "lost_free": d_free,
        "per_req": round(d_free / max(n, 1), 2),
        "lost_block": d_block,
        "alloc_blocks": d_blocks,
        "alloc_bytes": d_alloc,
        "worst_httpd_hw": worst_httpd,
        "before": before.__dict__,
        "after": after.__dict__,
    }
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "results.jsonl"), "a") as fh:
        fh.write(json.dumps(row) + "\n")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("surface", nargs="?", help="one of: " + ", ".join(SURFACES))
    ap.add_argument("-n", type=int, default=300)
    ap.add_argument("--settle", type=int, default=3, help="HEAPCSV lines to wait (15s each)")
    ap.add_argument("--warmup", type=int, default=5)
    ap.add_argument("--no-keepalive", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--probe", action="store_true", help="one call per surface, print status")
    args = ap.parse_args()

    if args.list:
        print("\n".join(SURFACES))
        return
    if args.probe:
        s = requests.Session()
        s.auth = AUTH
        for name, fn in SURFACES.items():
            if name == "idle":
                continue
            try:
                fn(s, 0)
                print(f"{name:28s} ok")
            except Exception as exc:  # noqa: BLE001
                print(f"{name:28s} FAIL {exc}")
        return
    if not args.surface:
        sys.exit("give a surface, or --list")
    run(args.surface, args.n, args.settle, not args.no_keepalive, args.warmup)


if __name__ == "__main__":
    main()
