#!/usr/bin/env python3
"""The deepest handler on the httpd task: the file browser's recursive delete and copy.

Both walk a directory tree with real recursion, bounded at 8 levels
(components/web_file_browser/web_file_browser.cpp:29). The staircase says a one-level
mkdir+delete already takes the httpd task to 3536 of its 4352 bytes, so this builds the
deepest tree the component accepts and watches what a full-depth walk costs.
"""
from __future__ import annotations

import argparse
import sys

import requests
from requests.auth import HTTPDigestAuth

import e2

S = requests.Session()
S.auth = HTTPDigestAuth("admin", "admin")


def post(path: str, **kw):
    kw.setdefault("timeout", 60)
    return S.post(e2.BASE + path, **kw)


def get(path: str, **kw):
    kw.setdefault("timeout", 60)
    return S.get(e2.BASE + path, **kw)


def build(depth: int, files_per_level: int) -> str:
    parts = []
    for level in range(depth):
        parts.append(f"L{level}")
        path = "/" + "/".join(parts)
        r = post("/files/mkdir", data={"path": path})
        if r.status_code != 200:
            print(f"mkdir {path}: {r.status_code} {r.text[:120]}")
            break
        for n in range(files_per_level):
            post(
                f"/files/write?path={path}/f{n}.txt",
                data=b"q" * 32,
                headers={"Content-Type": "text/plain"},
            )
    return "/" + parts[0]


def report(label: str) -> int:
    s = e2.wait_samples(1, label)
    print(f"{label:34s} httpd headroom {s.httpd_hw:5d} B  used {4352 - s.httpd_hw:5d} B  free={s.free}")
    return s.httpd_hw


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--depth", type=int, default=8)
    ap.add_argument("--files", type=int, default=3)
    args = ap.parse_args()

    report("before")
    root = build(args.depth, args.files)
    report(f"built {args.depth} levels")

    print(get("/files/list?path=/L0/L1/L2").text[:200])

    post("/files/copy", data={"old_path": root, "new_path": "/C0"})
    report("copy of the whole tree")

    post("/files/delete", data={"path": "/C0"})
    report("delete of the copy")

    r = post("/files/delete", data={"path": root})
    print(f"delete root -> {r.status_code} {r.text[:160]}")
    low = report("delete of the original")
    print(f"\nworst httpd headroom in this run: {low} B of 4352")


if __name__ == "__main__":
    sys.exit(main())
