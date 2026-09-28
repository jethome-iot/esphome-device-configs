#!/usr/bin/env python3
"""Independent frame check: read the `entry a1, N` immediate (plus any extra
`addi/addmi a1, a1, -N`) straight out of objdump -d of the .xt.prop-stripped
firmware.elf, and compare with graph.json and with the -fstack-usage corpus."""
import json, re, sys, csv
from pathlib import Path

BASE = Path("/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/"
            "bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack")
DIS = BASE / "firmware.dis"
G = json.loads((BASE / "graph.json").read_text())

WANT = [l.strip() for l in sys.stdin.read().splitlines() if l.strip()]
want = set(WANT)

FN = re.compile(r"^([0-9a-f]{8}) <(.+)>:$")
ENTRY = re.compile(r"\bentry\s+a1,\s*(0x[0-9a-f]+|\d+)")
ADJ = re.compile(r"\badd(?:mi|i(?:\.n)?)\s+a1,\s*a1,\s*(-?\d+)")
MOVSP = re.compile(r"\bmovsp\s+a1,")

cur = None
res = {}
for line in DIS.open(errors="replace"):
    m = FN.match(line.rstrip())
    if m:
        cur = m.group(2)
        if cur in want:
            res[cur] = {"addr": m.group(1), "entry": None, "extra": 0, "dyn": False}
        continue
    if cur in res:
        d = res[cur]
        if d["entry"] is None:
            e = ENTRY.search(line)
            if e:
                d["entry"] = int(e.group(1), 0)
                continue
        a = ADJ.search(line)
        if a and int(a.group(1)) < 0:
            d["extra"] += -int(a.group(1))
        if MOVSP.search(line):
            d["dyn"] = True

# -fstack-usage corpus, joined by demangled name
su = {}
with (BASE / "corpus/stack_usage.csv").open() as f:
    for row in csv.DictReader(f):
        if row.get("image") == "bootloader":
            continue
        su.setdefault(row["function"], set()).add((row["bytes"], row["qualifier"]))

print(f"{'entry':>6} {'extra':>5} {'graph':>6} {'.su':>12}  symbol")
bad = 0
for s in WANT:
    d = res.get(s)
    g = G.get(s, {}).get("frame")
    dm = G.get(s, {}).get("demangled", s)
    sv = su.get(dm) or su.get(s)
    svs = ",".join(f"{b}{q[:1]}" for b, q in sorted(sv)) if sv else "-"
    if d is None:
        print(f"{'?':>6} {'?':>5} {str(g):>6} {svs:>12}  MISSING IN DIS  {dm[:70]}")
        bad += 1
        continue
    flag = "" if d["entry"] == g else "  <<< MISMATCH"
    if d["dyn"]:
        flag += "  <<< DYNAMIC SP"
    if d["extra"]:
        flag += f"  <<< extra sp {d['extra']}"
    if flag:
        bad += 1
    print(f"{d['entry']:>6} {d['extra']:>5} {str(g):>6} {svs:>12}  {dm[:70]}{flag}")
print(f"\n{len(WANT)} symbols, {bad} flagged")
