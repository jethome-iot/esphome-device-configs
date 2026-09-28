#!/usr/bin/env python3
"""Parse `xtensa-esp32-elf-objdump -d --no-show-raw-insn firmware.elf` into
frame sizes + a direct-call graph, and write it as JSON.

Frame size comes from the windowed-ABI prologue `entry a1, N` (which equals the
GCC -fstack-usage number, verified on several functions), plus any extra
explicit stack-pointer adjustment inside the body.  `sub a1, a1, aN` / `movsp`
means a runtime-sized (VLA/alloca) frame and is flagged, not measured.
"""

import json
import re
import subprocess
import sys
from pathlib import Path

dis = Path(sys.argv[1])
out = Path(sys.argv[2])

FUNC = re.compile(r"^([0-9a-f]{8}) <(.+)>:$")
INSN = re.compile(r"^\s*[0-9a-f]+:\t(\S+)\s*(.*)$")
CALL = re.compile(r"^(call0|call4|call8|call12)\s+[0-9a-f]+ <([^>+]+)>")
JMP = re.compile(r"^j(?:\.l)?\s+[0-9a-f]+ <([^>+]+)>$")
NUM = r"(0x[0-9a-fA-F]+|\d+)"
ENTRY = re.compile(rf"^a1,\s*{NUM}$")
SPSUB = re.compile(rf"^a1,\s*a1,\s*-{NUM}$")
SPDYN = re.compile(r"^a1,\s*a1,\s*a\d+$")

funcs = {}
cur = None
for line in dis.open(encoding="utf-8", errors="replace"):
    m = FUNC.match(line.rstrip("\n"))
    if m:
        cur = {
            "addr": m.group(1),
            "sym": m.group(2),
            "entry": None,
            "extra_sp": 0,
            "dyn_sp": False,
            "calls": set(),
            "tails": set(),
            "indirect": 0,
            "insns": 0,
        }
        funcs[m.group(2)] = cur
        continue
    if cur is None:
        continue
    im = INSN.match(line.rstrip("\n"))
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
        cur["dyn_sp"] = True  # alloca / VLA: sp moved from a register
    elif op.startswith("call"):
        cm = CALL.match(f"{op} {args}")
        if cm:
            cur["calls"].add(cm.group(2))
        elif op.startswith("callx"):
            cur["indirect"] += 1
    elif op.startswith("j"):
        jm = JMP.match(f"{op} {args}")
        if jm and jm.group(1) != cur["sym"]:
            cur["tails"].add(jm.group(1))

# demangle every symbol in one pass; a GCC clone suffix ($part$0, $isra$0 ...)
# defeats c++filt, so it is stripped for the demangle and put back afterwards.
CLONE = re.compile(r"(\$(?:part|isra|constprop|cold)\$\d+)+$")
syms = list(funcs)
bases = [CLONE.sub("", sym) for sym in syms]
p = subprocess.run(
    [
        "/home/alexey/.cache/esphome/idf/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp32-elf-c++filt"
    ],
    input="\n".join(bases),
    capture_output=True,
    text=True,
    check=True,
)
demangled = {
    sym: dem + sym[len(base) :]
    for sym, base, dem in zip(syms, bases, p.stdout.splitlines())
}

data = {}
for s, f in funcs.items():
    data[s] = {
        "addr": f["addr"],
        "demangled": demangled.get(s, s),
        "frame": (f["entry"] or 0) + f["extra_sp"],
        "entry_imm": f["entry"],
        "extra_sp": f["extra_sp"],
        "dyn_sp": f["dyn_sp"],
        "indirect_calls": f["indirect"],
        "calls": sorted(f["calls"] | f["tails"]),
        "insns": f["insns"],
    }
out.write_text(json.dumps(data), encoding="utf-8")
noentry = [s for s, f in data.items() if f["entry_imm"] is None and f["insns"] > 2]
print(
    f"functions: {len(data)}  without `entry` prologue: {len(noentry)}  dynamic sp: {sum(1 for f in data.values() if f['dyn_sp'])}"
)
