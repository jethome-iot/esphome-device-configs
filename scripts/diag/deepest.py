#!/usr/bin/env python3
"""Deepest-stack path finder over the objdump disassembly.

Re-parses `xtensa-esp32-elf-objdump -d` (of the .xt.prop-stripped ELF, see
callgraph.py) keeping *call* edges and *tail-jump* edges apart, then reports the
most expensive reachable chain from a given entry symbol.

A `call4/8/12` rotates the register window and the callee's `entry aN, imm`
allocates `imm` bytes on top of the caller's frame, so a chain's cost is the sum
of the entry immediates (plus any extra explicit `addi a1,a1,-N`).  A `j` to
another symbol is GCC's hot/cold partition jump: the target runs *in place of*
the rest of the jumping function, so its frame replaces rather than adds --
unless the target has its own `entry`, in which case it is recorded and the
tail edge is charged like a call (conservative).

Cycles are reported, never silently unrolled: --unroll N charges each cycle N
times and the chain is labelled.
"""

import argparse
import json
import re
import sys
from pathlib import Path

FUNC = re.compile(r"^([0-9a-f]{8}) <(.+)>:$")
INSN = re.compile(r"^\s*[0-9a-f]+:\t(\S+)\s*(.*)$")
CALL = re.compile(r"^(call0|call4|call8|call12)\s+[0-9a-f]+ <([^>+]+)>")
JMP = re.compile(r"^j(?:\.l)?\s+[0-9a-f]+ <([^>+]+)>$")
NUM = r"(0x[0-9a-fA-F]+|\d+)"
ENTRY = re.compile(rf"^a1,\s*{NUM}$")
SPSUB = re.compile(rf"^a1,\s*a1,\s*-{NUM}$")
SPDYN = re.compile(r"^a1,\s*a1,\s*a\d+$")


def parse(dis_path):
    funcs, cur = {}, None
    for line in Path(dis_path).open(encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        m = FUNC.match(line)
        if m:
            cur = {
                "addr": m.group(1), "sym": m.group(2), "entry": None,
                "extra_sp": 0, "dyn_sp": False, "calls": [], "tails": [],
                "indirect": 0, "insns": 0,
            }
            funcs[m.group(2)] = cur
            continue
        if cur is None:
            continue
        im = INSN.match(line)
        if not im:
            continue
        op, args = im.group(1), im.group(2).strip()
        cur["insns"] += 1
        if op == "entry":
            em = ENTRY.match(args)
            if em and cur["entry"] is None:
                cur["entry"] = int(em.group(1), 0)
        elif op in ("addi", "addi.n", "addmi") and args.startswith("a1, a1, -"):
            sm = SPSUB.match(args)
            if sm:
                cur["extra_sp"] = max(cur["extra_sp"], int(sm.group(1), 0))
        elif op in ("sub", "subx2", "subx4", "subx8") and SPDYN.match(args):
            cur["dyn_sp"] = True
        elif op == "movsp" and args.startswith("a1,"):
            cur["dyn_sp"] = True
        elif op.startswith("call"):
            cm = CALL.match(f"{op} {args}")
            if cm:
                if cm.group(2) not in cur["calls"]:
                    cur["calls"].append(cm.group(2))
            elif op.startswith("callx"):
                cur["indirect"] += 1
        elif op.startswith("j"):
            jm = JMP.match(f"{op} {args}")
            if jm and jm.group(1) != cur["sym"] and jm.group(1) not in cur["tails"]:
                cur["tails"].append(jm.group(1))
    for f in funcs.values():
        f["frame"] = (f["entry"] or 0) + f["extra_sp"]
    return funcs


def build(funcs, extra_edges):
    """Edges: (callee, charged) -- charged False for a frameless tail target."""
    g = {}
    for s, f in funcs.items():
        edges = []
        for c in f["calls"]:
            if c in funcs:
                edges.append((c, True))
        for t in f["tails"]:
            if t in funcs:
                # a partition/cold target with no `entry` of its own runs on this frame
                edges.append((t, funcs[t]["entry"] is not None))
        for c in extra_edges.get(s, []):
            if c in funcs and not any(e[0] == c for e in edges):
                edges.append((c, True))
        g[s] = edges
    return g


def deepest(funcs, g, entry, unroll=1, seen_limit=None):
    """Iterative DFS keeping the max-cost simple path; cycles charged `unroll` times."""
    best = {"cost": -1, "path": []}
    cycles = set()
    memo = {}

    def rec(sym, stack, depth):
        f = funcs[sym]
        own = f["frame"]
        key = sym
        # memo is only valid when the subtree contains no node on the current stack
        if key in memo and not (memo[key][1] & set(stack)):
            return memo[key]
        bestsub, touched = 0, set()
        bestpath = []
        for callee, charged in g.get(sym, []):
            if callee in stack:
                cycles.add((sym, callee))
                continue
            if depth > 220:
                continue
            sub, tset, spath = rec(callee, stack | {callee}, depth + 1)
            cost = (sub if charged else max(0, sub - funcs[callee]["frame"]))
            if cost > bestsub:
                bestsub, bestpath = cost, spath
            touched |= tset
        res = (own + bestsub, touched | {sym}, [(sym, own)] + bestpath)
        memo[key] = res
        return res

    sys.setrecursionlimit(30000)
    cost, _t, path = rec(entry, {entry}, 0)
    return cost, path, cycles


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dis")
    ap.add_argument("entries", nargs="+")
    ap.add_argument("--seed", action="append", default=[],
                    help="CALLER=CALLEE extra (indirect) edge; repeatable")
    ap.add_argument("--cache", default=None)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()

    if a.cache and Path(a.cache).exists():
        funcs = json.loads(Path(a.cache).read_text())
    else:
        funcs = parse(a.dis)
        if a.cache:
            Path(a.cache).write_text(json.dumps(funcs))

    extra = {}
    for s in a.seed:
        k, v = s.split("=", 1)
        extra.setdefault(k, []).append(v)
    g = build(funcs, extra)

    out = []
    for e in a.entries:
        if e not in funcs:
            print(f"!! not a symbol: {e}")
            continue
        cost, path, cycles = deepest(funcs, g, e)
        print(f"\n=== {e}  ({funcs[e]['addr']})  worst-case {cost} B, {len(path)} frames")
        run = 0
        for sym, own in path:
            run += own
            d = "  DYN" if funcs[sym]["dyn_sp"] else ""
            ic = funcs[sym]["indirect"]
            print(f"  {own:>5} {run:>6}  {sym}{d}{'  [callx x%d]' % ic if ic else ''}")
        if cycles:
            print("  cycles (not unrolled):")
            for c in sorted(cycles):
                print(f"    {c[0]} -> {c[1]}")
        out.append({"entry": e, "cost": cost, "path": path,
                    "cycles": sorted(cycles)})
    if a.json:
        Path(a.json).write_text(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
