#!/usr/bin/env python3
"""At what directory depth does the file browser's recursive walk run out of httpd stack.

Builds one level at a time and, after each, runs the recursive operations on the tree so far,
printing the httpd task's stack headroom. The run ends when the device stops answering.
"""
from __future__ import annotations

import argparse
import sys
import time

import requests
from requests.auth import HTTPDigestAuth

import e2

S = requests.Session()
S.auth = HTTPDigestAuth("admin", "admin")


def alive() -> bool:
    try:
        return S.get(e2.BASE + "/automation-editor/api/ping", timeout=10).status_code == 200
    except Exception:  # noqa: BLE001
        return False


def op(path: str, data: dict, label: str) -> str:
    try:
        r = S.post(e2.BASE + path, data=data, timeout=45)
        return f"{label}={r.status_code}"
    except Exception as exc:  # noqa: BLE001
        return f"{label}=DEAD({type(exc).__name__})"


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-depth", type=int, default=8)
    ap.add_argument("--op", choices=["copy", "delete", "both"], default="both")
    args = ap.parse_args()

    print(f"start: httpd headroom {e2.last_sample().httpd_hw} B of 4352")
    parts = []
    for level in range(args.max_depth):
        parts.append(f"L{level}")
        path = "/" + "/".join(parts)
        for lvl in range(level + 1):
            S.post(
                e2.BASE + "/files/mkdir",
                data={"path": "/" + "/".join(parts[: lvl + 1])},
                timeout=45,
            )
        S.post(
            e2.BASE + f"/files/write?path={path}/f.txt",
            data=b"q" * 32,
            headers={"Content-Type": "text/plain"},
            timeout=45,
        )
        depth = level + 1
        results = []
        if args.op in ("copy", "both"):
            results.append(op("/files/copy", {"old_path": "/L0", "new_path": "/C0"}, "copy"))
            results.append(op("/files/delete", {"path": "/C0"}, "rm-copy"))
        if args.op in ("delete", "both"):
            # The tree is rebuilt on the next round, so deleting it here is the point.
            results.append(op("/files/delete", {"path": "/L0"}, "delete"))
        if not alive():
            print(f"depth {depth}: device stopped answering  ({' '.join(results)})")
            time.sleep(25)
            print("  device back:", alive())
            break
        s = e2.wait_samples(1, f"depth {depth}")
        print(
            f"depth {depth}: httpd headroom {s.httpd_hw:5d} B  used {4352 - s.httpd_hw:5d} B  "
            f"free={s.free}  {' '.join(results)}"
        )


if __name__ == "__main__":
    sys.exit(main())
