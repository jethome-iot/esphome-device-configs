#!/usr/bin/env python3
"""Sum an explicit, hand-verified chain; print each frame from all three sources."""
import csv, json, re, sys
from pathlib import Path
BASE=Path("/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack/slice-upload")
CORP=BASE.parent/"corpus"
O=json.loads((BASE/"objgraph.json").read_text())
G=json.loads((BASE/"graph.json").read_text())
su={}
with (CORP/"stack_usage.csv").open() as f:
    for row in csv.DictReader(f):
        if row["image"]!="app": continue
        su.setdefault(row["function"], set()).add(int(row["bytes"]))
def sufor(sym):
    d = O.get(sym,{}).get('demangled') or G.get(sym,{}).get('demangled') or sym
    # .su pretty names: strip [abi:cxx11], template detail
    cands=[d, d.replace("[abi:cxx11]",""), re.sub(r"\$(?:part|isra|constprop|cold)\$\d+$","",d)]
    for c in cands:
        if c in su: return sorted(su[c])
    return None
def run(name, chain):
    tot=0; print(f"\n===== {name}")
    print(f"{'frame':>6} {'run':>6}  {'obj':>4} {'elf':>4} {'.su':>10}  symbol")
    for s in chain:
        of=O.get(s,{}).get('frame'); gf=G.get(s,{}).get('frame'); sf=sufor(s)
        f = of if of is not None else (gf or 0)
        tot+=f
        d = O.get(s,{}).get('demangled') or G.get(s,{}).get('demangled') or s
        print(f"{f:6d} {tot:6d}  {str(of):>4} {str(gf):>4} {str(sf):>10}  {d[:96]}")
    print(f"TOTAL {tot}")
    return tot
