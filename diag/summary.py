#!/usr/bin/env python3
"""Print the results.jsonl rows as one table, worst first."""
from __future__ import annotations

import json
import os

path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "results.jsonl")
rows = [json.loads(ln) for ln in open(path) if ln.strip()]

hdr = f"{'surface':28s} {'n':>5s} {'ka':>3s} {'B/req':>8s} {'lost B':>8s} {'blk/req':>8s} {'req/s':>7s} {'err':>4s}"
print(hdr)
print("-" * len(hdr))
for r in sorted(rows, key=lambda r: -r["per_req"]):
    rate = r["n"] / r["seconds"] if r["seconds"] else 0.0
    print(
        f"{r['surface']:28s} {r['n']:5d} {'y' if r['keepalive'] else 'n':>3s} "
        f"{r['per_req']:8.2f} {r['lost_free']:8d} "
        f"{r['alloc_blocks'] / max(r['n'], 1):8.3f} {rate:7.1f} {r['errors']:4d}"
    )
