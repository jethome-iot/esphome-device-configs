#!/usr/bin/env python3
"""Light, realistic traffic plus a heap trace, so the slope can be extrapolated to hours.

Traffic profile: what one open dashboard tab does — a persistent /events SSE connection
that is actually read, /api/device/status and /api/device/network every 5 s, and the whole
page once a minute. Heap numbers come from the firmware's own HEAPCSV lines, so the CSV is
written from the QEMU log rather than from extra requests.
"""
from __future__ import annotations

import argparse
import csv
import os
import re
import sys
import threading
import time

import requests
from requests.auth import HTTPDigestAuth

BASE = os.environ.get("E2_BASE", "http://127.0.0.1:8091")
LOG = os.environ.get(
    "E2_LOG",
    "/home/alexey/dev/wt-esphome-device-configs/heap-qemu/devices/JXD/"
    ".esphome/build/jxd-r6-e1eth-lcd-qemu/qemu.log",
)
AUTH = HTTPDigestAuth("admin", "admin")
HEAPCSV = re.compile(rb"HEAPCSV,(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)")
COLUMNS = ["wall", "ms", "free", "block", "minfree", "alloc_b", "alloc_n", "free_n", "psram"]
stop = threading.Event()


def poller() -> None:
    s = requests.Session()
    s.auth = AUTH
    n = 0
    while not stop.wait(5):
        n += 1
        for path in ("/api/device/status", "/api/device/network"):
            try:
                s.get(BASE + path, timeout=20)
            except Exception:  # noqa: BLE001
                pass
        if n % 12 == 0:
            try:
                s.get(BASE + "/", timeout=60)
            except Exception:  # noqa: BLE001
                pass


def sse_reader() -> None:
    """One dashboard tab: connect, read the stream, reconnect if it drops."""
    s = requests.Session()
    s.auth = AUTH
    while not stop.is_set():
        try:
            r = s.get(BASE + "/events", stream=True, timeout=30)
            for _ in r.iter_lines():
                if stop.is_set():
                    break
            r.close()
        except Exception:  # noqa: BLE001
            time.sleep(2)


def tracer(path: str) -> None:
    seen = 0
    new = not os.path.exists(path)
    with open(path, "a", newline="") as fh:
        w = csv.writer(fh)
        if new:
            w.writerow(COLUMNS)
        while not stop.wait(5):
            with open(LOG, "rb") as lf:
                lf.seek(0, os.SEEK_END)
                size = lf.tell()
                lf.seek(max(0, size - 2_000_000))
                rows = HEAPCSV.findall(lf.read())
            for row in rows[seen:]:
                w.writerow([round(time.time(), 1), *(int(v) for v in row)])
            seen = len(rows)
            fh.flush()


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=60.0)
    ap.add_argument("--csv", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "soak.csv"))
    ap.add_argument("--traffic", choices=["light", "none"], default="light")
    args = ap.parse_args()

    threads = [threading.Thread(target=tracer, args=(args.csv,), daemon=True)]
    if args.traffic == "light":
        threads += [
            threading.Thread(target=poller, daemon=True),
            threading.Thread(target=sse_reader, daemon=True),
        ]
    for t in threads:
        t.start()
    print(f"soak: traffic={args.traffic} for {args.minutes} min -> {args.csv}", flush=True)
    try:
        time.sleep(args.minutes * 60)
    except KeyboardInterrupt:
        pass
    stop.set()
    time.sleep(1)
    print("soak: done", flush=True)


if __name__ == "__main__":
    sys.exit(main())
