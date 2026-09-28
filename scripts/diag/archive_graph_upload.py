#!/usr/bin/env python3
"""Frame sizes + direct-call edges read from the *object files* inside the build's
static archives, where objdump decodes cleanly and every call carries a
relocation naming its target.  Immune to the linked-image decode slips."""
import json, re, subprocess, sys
from pathlib import Path

OD = "/home/alexey/.cache/esphome/idf/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump"
CXXFILT = "/home/alexey/.cache/esphome/idf/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp32-elf-c++filt"

SEC = re.compile(r"^Disassembly of section (\S+):$")
SYM = re.compile(r"^([0-9a-f]+) <(.+)>:$")
INSN = re.compile(r"^\s*([0-9a-f]+):\t([0-9a-f ]+)\t\s*(\S+)\s*(.*)$")
RELOC = re.compile(r"^\s+([0-9a-f]+): (R_XTENSA_\S+)\s+(.+)$")
NUM = r"(0x[0-9a-fA-F]+|\d+)"
ENTRY = re.compile(rf"^a1,\s*{NUM}$")
SPSUB = re.compile(rf"^a1,\s*a1,\s*-{NUM}$")

def parse(archives):
    out = {}
    for ar in archives:
        p = subprocess.run([OD, "-dr", str(ar)], capture_output=True, text=True)
        member = None; sec = None; cur = None
        pend = []   # relocations seen, keyed by offset
        relocs = {} # offset -> list of (type, target)
        lines = p.stdout.splitlines()
        for line in lines:
            if line.endswith(":\tfile format elf32-xtensa-le"):
                member = line.split(":")[0]
                continue
            m = SEC.match(line)
            if m:
                sec = m.group(1); cur = None; relocs = {}
                continue
            m = SYM.match(line)
            if m:
                name = m.group(2)
                if sec and (sec.startswith(".text") or sec.startswith(".iram")):
                    cur = out.setdefault(name, dict(entry=None, extra_sp=0, dyn=False,
                                                    calls=set(), indirect=0, insns=0,
                                                    ar=Path(ar).name, member=member, sec=sec))
                else:
                    cur = None
                continue
            m = RELOC.match(line)
            if m and cur is not None:
                off, typ, tgt = int(m.group(1), 16), m.group(2), m.group(3)
                relocs.setdefault(off, []).append((typ, tgt))
                # a relaxed call: the l32r/call carries ASM_EXPAND naming the callee
                if typ == "R_XTENSA_ASM_EXPAND":
                    t = tgt.split("+")[0]
                    # the target is often a SECTION name (.text.<sym>, .iram1.<sym>)
                    m2 = re.match(r"^\.(?:text|iram1|iram\.text|flash\.text|literal)\.(.+)$", t)
                    if m2: t = m2.group(1)
                    if not t.startswith("."):
                        cur["calls"].add(t)
                if typ == "R_XTENSA_SLOT0_OP" and not tgt.startswith("."):
                    cur["_slot0"] = cur.get("_slot0", set()) | {tgt.split("+")[0]}
                continue
            m = INSN.match(line)
            if m and cur is not None:
                off, op, args = int(m.group(1), 16), m.group(3), m.group(4).strip()
                cur["insns"] += 1
                if op == "entry":
                    em = ENTRY.match(args)
                    if em and cur["entry"] is None: cur["entry"] = int(em.group(1), 0)
                elif op in ("addi", "addi.n", "addmi") and args.startswith("a1, a1, -"):
                    sm = SPSUB.match(args)
                    if sm: cur["extra_sp"] = max(cur["extra_sp"], int(sm.group(1), 0))
                elif op in ("sub","subx2","subx4","subx8") and re.match(r"^a1,\s*a1,\s*a\d+$", args):
                    cur["dyn"] = True
                elif op == "movsp" and args.startswith("a1,"):
                    cur["dyn"] = True
                elif op.startswith("callx"):
                    # a relaxed direct call is l32r+callx with ASM_EXPAND on the l32r;
                    # count as indirect only when no ASM_EXPAND sits within 8 bytes before
                    near = any(any(t[0] == "R_XTENSA_ASM_EXPAND" for t in relocs.get(o, []))
                               for o in range(max(0, off-8), off+1))
                    if not near: cur["indirect"] += 1
                elif op.startswith("call") and "<" in m.group(4):
                    t = re.search(r"<([^>+]+)>", m.group(4))
                    if t: cur["calls"].add(t.group(1))
    for k, v in out.items():
        v["calls"] = sorted(v["calls"])
        v.pop("_slot0", None)
        v["frame"] = (v["entry"] or 0) + v["extra_sp"]
    return out

if __name__ == "__main__":
    archives = sys.argv[2:]
    data = parse(archives)
    syms = list(data)
    p = subprocess.run([CXXFILT], input="\n".join(syms), capture_output=True, text=True, check=True)
    for s, d in zip(syms, p.stdout.splitlines()): data[s]["demangled"] = d
    Path(sys.argv[1]).write_text(json.dumps(data))
    print("symbols:", len(data))
