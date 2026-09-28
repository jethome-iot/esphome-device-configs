#!/usr/bin/env python3
"""httpd stack head-room per POST path, and for the deepest URLs the components accept.

uxTaskGetStackHighWaterMark is monotone, so this is a staircase: paths run one at a time from
the shallowest expected to the deepest, and every new low belongs to the path that just ran.
The recursive copy/delete paths are measured separately by depth.py, which would otherwise
mask everything after them.
"""
from __future__ import annotations

import argparse
import io
import sys

import requests
from requests.auth import HTTPDigestAuth

import e2

S = requests.Session()
S.auth = HTTPDigestAuth("admin", "admin")
B = e2.BASE


def post(path: str, **kw):
    kw.setdefault("timeout", 120)
    return S.post(B + path, **kw)


def get(path: str, **kw):
    kw.setdefault("timeout", 120)
    return S.get(B + path, **kw)


def upload(size: int):
    return post(f"/files/upload?path=/u.bin", files={"file": ("u.bin", b"m" * size)})


def write(size: int):
    return post(
        "/files/write?path=/w.bin",
        data=b"w" * size,
        headers={"Content-Type": "application/octet-stream"},
    )


RULE = {
    "name": "PLACEHOLDER",
    "enabled": False,
    "mode": "single",
    "triggers": [{"source": "input", "type": "click", "object_id": "input_1"}],
    "actions": [
        {"source": "switch", "type": "turn_on", "object_id": "red_led"},
        {"source": "delay", "delay_ms": 1000},
        {"source": "switch", "type": "turn_off", "object_id": "red_led"},
    ],
}


def save_rules(n: int):
    import json

    last = None
    for i in range(n):
        body = dict(RULE, name=f"rule{i:03d}")
        last = post(
            "/automation-editor/api/save",
            data=json.dumps(body),
            headers={"Content-Type": "application/json"},
        )
    return last


def deep_url(component: str, target_len: int = 500):
    """A URL close to the 513-byte buffer each component keeps on the httpd stack."""
    if component == "files":
        base = "/files/read?path=/"
        pad = "a" * max(0, target_len - len(base))
        return get(base + pad)
    if component == "dashboard":
        base = "/api/device/"
        pad = "b" * max(0, target_len - len(base))
        return get(base + pad)
    base = "/automation-editor/api/"
    pad = "c" * max(0, target_len - len(base))
    return get(base + pad)


STEPS = [
    ("boot baseline (one ping)", lambda: get("/automation-editor/api/ping")),
    ("GET /", lambda: get("/")),
    ("GET /api/device/entities", lambda: get("/api/device/entities")),
    ("GET /files/list?path=/", lambda: get("/files/list?path=/")),
    ("deep URL 500 B on /files", lambda: deep_url("files")),
    ("deep URL 500 B on /api/device", lambda: deep_url("dashboard")),
    ("deep URL 500 B on /automation-editor", lambda: deep_url("editor")),
    ("POST /files/write 256 B", lambda: write(256)),
    ("POST /files/write 64 KB", lambda: write(64 * 1024)),
    ("POST /files/write 512 KB", lambda: write(512 * 1024)),
    ("POST /files/upload 256 B multipart", lambda: upload(256)),
    ("POST /files/upload 64 KB multipart", lambda: upload(64 * 1024)),
    ("POST /files/upload 512 KB multipart", lambda: upload(512 * 1024)),
    ("POST /files/mkdir", lambda: post("/files/mkdir", data={"path": "/ps"})),
    ("POST /files/rename", lambda: post("/files/rename", data={"old_path": "/ps", "new_path": "/ps2"})),
    ("POST /files/delete (empty dir)", lambda: post("/files/delete", data={"path": "/ps2"})),
    ("POST /api/device/entity-settings", lambda: post(
        "/api/device/entity-settings",
        data='{"type":"switch","source_name":"relay_1","settings":{"restore_mode":"RESTORE_DEFAULT_OFF"}}',
        headers={"Content-Type": "application/json"})),
    ("POST /api/device/auth", lambda: post(
        "/api/device/auth", data='{"username":"admin","password":"admin"}',
        headers={"Content-Type": "application/json"})),
    ("POST rule save x 20", lambda: save_rules(20)),
    ("GET /automation-editor/api/list (20 rules)", lambda: get("/automation-editor/api/list")),
    ("GET /automation-editor/api/export (20 rules)", lambda: get("/automation-editor/api/export")),
    ("POST rule save x 30 more", lambda: save_rules(30)),
    ("GET /automation-editor/api/export (50 rules)", lambda: get("/automation-editor/api/export")),
]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=3)
    args = ap.parse_args()

    low = e2.last_sample().httpd_hw
    print(f"start: httpd head-room {low} B of 4352\n")
    print(f"{'path':44s} {'head-room':>10s} {'used':>7s}  status")
    for label, fn in STEPS:
        status = ""
        for _ in range(args.reps):
            try:
                r = fn()
                status = str(getattr(r, "status_code", "?"))
            except Exception as exc:  # noqa: BLE001
                status = f"DEAD({type(exc).__name__})"
                break
        s = e2.wait_samples(1, label)
        mark = ""
        if s.httpd_hw < low:
            mark = f"  <-- new low, -{low - s.httpd_hw} B"
            low = s.httpd_hw
        print(f"{label:44s} {s.httpd_hw:10d} {4352 - s.httpd_hw:7d}  {status}{mark}")
    print(f"\nworst httpd head-room: {low} B of 4352 ({100 * (4352 - low) / 4352:.0f}% used)")


if __name__ == "__main__":
    sys.exit(main())
