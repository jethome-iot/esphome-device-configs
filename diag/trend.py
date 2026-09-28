#!/usr/bin/env python3
"""Least-squares slope of the internal-heap trace in the QEMU log, with R².

The trace is the firmware's own HEAPCSV lines, so it costs no requests and covers whatever the
device was doing. Optionally restricts to a window, so an idle stretch and a traffic stretch
can be fitted separately.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

LOG = os.environ.get(
    "E2_LOG",
    "/home/alexey/dev/wt-esphome-device-configs/heap-qemu/devices/JXD/"
    ".esphome/build/jxd-r6-e1eth-lcd-qemu/qemu.log",
)
ROW = re.compile(rb"HEAPCSV," + rb",".join([rb"(\d+)"] * 11))


def fit(xs: list[float], ys: list[float]) -> tuple[float, float, float]:
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return 0.0, my, 1.0
    slope = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    intercept = my - slope * mx
    ss_tot = sum((y - my) ** 2 for y in ys)
    ss_res = sum((y - (slope * x + intercept)) ** 2 for x, y in zip(xs, ys))
    r2 = 1.0 if ss_tot == 0 else 1.0 - ss_res / ss_tot
    return slope, intercept, r2


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--from-min", type=float, default=0.0, help="skip the first N minutes")
    ap.add_argument("--to-min", type=float, default=1e9)
    ap.add_argument("--budget", type=float, default=None, help="bytes to exhaust; default = last free")
    args = ap.parse_args()

    with open(LOG, "rb") as fh:
        rows = [[int(v) for v in m.groups()] for m in ROW.finditer(fh.read())]
    if len(rows) < 5:
        sys.exit("not enough HEAPCSV rows")

    # A reboot restarts millis(); keep only the last boot's rows.
    start = 0
    for i in range(1, len(rows)):
        if rows[i][0] < rows[i - 1][0]:
            start = i
    rows = rows[start:]

    t0 = rows[0][0]
    sel = [r for r in rows if args.from_min * 60000 <= r[0] - t0 <= args.to_min * 60000]
    if len(sel) < 5:
        sys.exit(f"only {len(sel)} rows in that window")

    hours = [(r[0] - t0) / 3_600_000 for r in sel]
    for idx, label in ((1, "internal free"), (2, "largest block"), (5, "allocated blocks")):
        slope, _, r2 = fit(hours, [float(r[idx]) for r in sel])
        unit = "blocks" if idx == 5 else "B"
        print(f"{label:17s} {slope:+10.1f} {unit}/h   R2={r2:.3f}   "
              f"first={sel[0][idx]} last={sel[-1][idx]}")

    free_slope, _, _ = fit(hours, [float(r[1]) for r in sel])
    budget = args.budget if args.budget is not None else float(sel[-1][1])
    span = (sel[-1][0] - sel[0][0]) / 3_600_000
    print(f"\nwindow {span:.2f} h, {len(sel)} samples, internal free now {sel[-1][1]} B")
    print(f"worst httpd stack headroom in window: {min(r[8] for r in sel)} B of 4352")
    print(f"worst loop stack headroom in window:  {min(r[9] for r in sel)} B")
    if free_slope < -1:
        print(f"at {-free_slope:.0f} B/h, {budget:.0f} B lasts {budget / -free_slope:.1f} h")
    else:
        print(f"no downward trend ({free_slope:+.1f} B/h): nothing to extrapolate")


if __name__ == "__main__":
    main()
