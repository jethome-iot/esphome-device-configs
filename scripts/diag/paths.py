#!/usr/bin/env python3
"""Deepest-stack path finder over the resynchronised call graph (resync.py).

Cost model (Xtensa windowed ABI): `call4/8/12` rotates the register window and
the callee's `entry aN, imm` allocates `imm` bytes below the caller's frame, so a
chain costs the sum of the entry immediates plus any explicit `addi a1,a1,-N`.
A `j` to another symbol is GCC's hot/cold partition jump: if the target has no
`entry` of its own it runs on the jumping function's frame and adds nothing;
if it does have one it is charged like a call.

Indirect calls (`callx8` off a vtable, std::function, a registered callback)
are edges the disassembly cannot resolve.  They are supplied with --seed
CALLER=CALLEE and are the only reason a handler tree is reachable at all.

--prune drops a subtree (abort/panic, logging) so it can be priced separately.
Recursion is reported, never unrolled: --loop N charges a self-recursive
function N times.
"""

import argparse
import json
import sys
from pathlib import Path

DEFAULT_PRUNE = [
    "panic_abort", "esp_system_abort", "abort", "__wrap_abort",
    "_esp_error_check_failed", "esp_restart", "__assert_func",
]


def load(path):
    g = json.loads(Path(path).read_text())
    for d in g.values():
        d.setdefault("tails", [])
    return g


def edges(g, sym, extra):
    out = []
    d = g.get(sym)
    if not d:
        return out
    for c in d["calls"]:
        if c in g:
            out.append((c, True))
    for t in d["tails"]:
        if t in g:
            out.append((t, g[t]["entry"] is not None))
    for c in extra.get(sym, []):
        if c in g and not any(e[0] == c for e in out):
            out.append((c, True))
    return out


def deepest(g, entry, extra, prune, loop=1):
    """Max-cost simple path.  Returns (cost, [(sym, frame)], cycles, dynamics)."""
    sys.setrecursionlimit(100000)
    cycles, dynamics = set(), set()
    memo = {}

    def rec(sym, onstack):
        d = g[sym]
        own = d["frame"] * (loop if sym in onstack else 1)
        if d["dyn_sp"]:
            dynamics.add(sym)
        hit = memo.get(sym)
        if hit is not None and not (hit[1] & onstack):
            return hit
        best, bpath, touched = 0, [], set()
        for callee, charged in edges(g, sym, extra):
            if callee in prune:
                continue
            if callee in onstack:
                cycles.add((sym, callee))
                continue
            sub, tset, spath = rec(callee, onstack | {callee})
            cost = sub if charged else max(0, sub - g[callee]["frame"])
            if cost > best:
                best, bpath = cost, spath
            touched |= tset
        res = (d["frame"] + best, touched | {sym}, [(sym, d["frame"])] + bpath)
        memo[sym] = res
        return res

    cost, _t, path = rec(entry, {entry})
    return cost, path, cycles, dynamics


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("graph")
    ap.add_argument("entries", nargs="+")
    ap.add_argument("--seed", action="append", default=[])
    ap.add_argument("--seed-file", default=None)
    ap.add_argument("--prune", action="append", default=[])
    ap.add_argument("--base", type=int, default=0,
                    help="bytes already on the stack above the entry")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    g = load(a.graph)
    extra = {}
    seeds = list(a.seed)
    if a.seed_file:
        seeds += [l.strip() for l in Path(a.seed_file).read_text().splitlines()
                  if l.strip() and not l.startswith("#")]
    for s in seeds:
        k, v = s.split("=", 1)
        extra.setdefault(k, []).append(v)
    prune = set(DEFAULT_PRUNE) | set(a.prune)

    res = []
    for e in a.entries:
        if e not in g:
            print(f"!! not a symbol: {e}")
            continue
        cost, path, cyc, dyn = deepest(g, e, extra, prune)
        print(f"\n=== {g[e].get('demangled', e)}")
        print(f"    worst case {cost + a.base} B ({cost} below the entry"
              f"{f' + {a.base} above it' if a.base else ''}), {len(path)} frames")
        run = a.base
        for sym, own in path:
            run += own
            tag = "  DYN" if g[sym]["dyn_sp"] else ""
            ic = g[sym]["indirect"]
            print(f"  {own:>5} {run:>6}  {g[sym].get('demangled', sym)[:110]}{tag}"
                  f"{'  [callx x%d]' % ic if ic else ''}")
        if cyc:
            print("  recursion / cycles NOT unrolled:")
            for c in sorted(cyc):
                print(f"    {c[0]} -> {c[1]}")
        if dyn:
            print(f"  runtime-sized frames on the subtree: {sorted(dyn)}")
        res.append({"entry": e, "cost": cost + a.base, "frames": len(path),
                    "path": path, "cycles": sorted(cyc), "dyn": sorted(dyn)})
    if a.json:
        Path(a.json).write_text(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
