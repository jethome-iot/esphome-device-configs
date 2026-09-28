#!/usr/bin/env python3
"""Rebuild the frame-size + call-graph corpus from a *resynchronised* disassembly.

Why this exists: a single linear `objdump -d` pass over an Xtensa image is wrong.
GCC pads to a 4-byte boundary before an aligned branch target, and with the
`.xt.prop` property table removed (which objdump needs but mis-reads on this
image, see callgraph.py) nothing tells the linear decoder that those two bytes
are padding.  It swallows them into an instruction and the stream desynchronises
until the next lucky re-alignment -- 2377 of the 8433 functions in this firmware
have at least one intra-function branch target that the linear dump does not
land on, and real `call8` edges are lost inside those windows
(`httpd_sess_process -> httpd_req_new` among them).

Fix: padding can only sit immediately before a branch target, so decoding each
[anchor, next-anchor) span separately is exact.  Anchors are the function entry
plus every intra-function branch target; the set grows as new spans decode, so
this iterates to a fixpoint.  One objdump invocation per desynchronised anchor
(~4 ms each), merged by discarding any instruction that straddles an anchor.

Output: same shape as callgraph.json, plus `tails` kept separate from `calls`
and a `resync` count per function.
"""

import bisect
import json
import re
import subprocess
import sys
from pathlib import Path

TOOLS = "/home/alexey/.cache/esphome/idf/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin"
OBJDUMP = f"{TOOLS}/xtensa-esp32-elf-objdump"
CXXFILT = f"{TOOLS}/xtensa-esp32-elf-c++filt"
READELF = f"{TOOLS}/xtensa-esp32-elf-readelf"

INSN = re.compile(r"^\s*([0-9a-f]+):\t(\S+)\s*(.*)$")
TGT = re.compile(r"\b([0-9a-f]{8}) <[^>]+>")
CALL = re.compile(r"^(call0|call4|call8|call12)\s+[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>")
JMP = re.compile(r"^j(?:\.l)?\s+[0-9a-f]+ <([^>+]+)(\+0x[0-9a-f]+)?>$")
# `l32r a8, <lit> (4005da7c <esp_rom_md5_init>)`: objdump resolves the literal's
# value to a symbol, so the `callx8 a8` that follows is a *direct* call in all but
# encoding -- a ROM entry, an IRAM libc routine, a cross-section thunk.  Without
# this the graph loses 31 edges out of `authenticate` alone.
L32R = re.compile(r"^(a\d+),\s*[0-9a-f]+ <[^>]*>\s+\([0-9a-f]+ <([A-Za-z_$][^>+]*)>\)$")
CALLX = re.compile(r"^(a\d+)$")
NUM = r"(0x[0-9a-fA-F]+|\d+)"
ENTRY = re.compile(rf"^a1,\s*{NUM}$")
SPSUB = re.compile(rf"^a1,\s*a1,\s*-{NUM}$")
SPDYN = re.compile(r"^a1,\s*a1,\s*a\d+$")
BRANCH = re.compile(r"^(b|j|loop)")


def funcs_from_elf(elf):
    """(name, addr, size) for every sized FUNC symbol, deduplicated by address."""
    out = subprocess.run([READELF, "-sW", elf], capture_output=True, text=True,
                         check=True).stdout
    row = re.compile(
        r"^\s*\d+:\s+([0-9a-f]+)\s+(\d+)\s+FUNC\s+\S+\s+\S+\s+\S+\s+(\S+)\s*$")
    seen = {}
    for line in out.splitlines():
        m = row.match(line)
        if not m:
            continue
        addr, size, name = int(m.group(1), 16), int(m.group(2)), m.group(3)
        if size == 0 or addr == 0:
            continue
        if addr not in seen or len(name) < len(seen[addr][0]):
            seen[addr] = (name, size)
    return [(n, a, s) for a, (n, s) in sorted(seen.items())]


def dump(elf, lo, hi):
    """objdump one span -> [(addr, op, args)] in address order."""
    out = subprocess.run(
        [OBJDUMP, "-d", f"--start-address=0x{lo:x}", f"--stop-address=0x{hi:x}",
         "--no-show-raw-insn", elf],
        capture_output=True, text=True, check=True).stdout
    res = []
    for line in out.splitlines():
        m = INSN.match(line)
        if m:
            res.append((int(m.group(1), 16), m.group(2), m.group(3).strip()))
    return res


def decode(elf, start, size, seed=()):
    """Resynchronised instruction stream for one function.

    Anchors are true instruction starts, so no real instruction straddles one.
    Decode linearly; the moment an instruction would cover an anchor, those
    bytes are alignment padding -- skip to the anchor and decode again from
    there.  That is one objdump call per padding spot, not per anchor.
    """
    end = start + size
    anchors = sorted({a for a in seed if start < a < end})
    stream, ncalls = {}, 0
    for _round in range(6):
        stream, pos, ncalls_r = {}, start, 0
        guard = 0
        while pos < end and guard < 400:
            guard += 1
            got = dump(elf, pos, end)
            ncalls_r += 1
            advanced = False
            nxt = None
            for i, (addr, op, args) in enumerate(got):
                nend = got[i + 1][0] if i + 1 < len(got) else end
                if addr != pos or addr >= end:
                    break
                j = bisect.bisect_right(anchors, addr)
                if j < len(anchors) and anchors[j] < nend:
                    nxt = anchors[j]   # padding: [addr, nxt) is not an instruction
                    break
                stream[addr] = (op, args)
                pos = nend
                advanced = True
            if nxt is not None:
                pos = nxt
                continue
            if not advanced:
                break
        ncalls += ncalls_r
        new = set()
        for addr, (op, args) in stream.items():
            if BRANCH.match(op) and not op.startswith("break"):
                for t in TGT.findall(args):
                    t = int(t, 16)
                    if start < t < end and t not in stream:
                        new.add(t)
        if not new:
            break
        anchors = sorted(set(anchors) | new)
    return stream, anchors, ncalls


def analyse(name, stream):
    entry, extra, dyn = None, 0, False
    calls, tails, indirect = [], [], 0
    litcalls, reg = [], {}
    for addr in sorted(stream):
        op, args = stream[addr]
        if op == "l32r":
            m = L32R.match(args)
            reg[args.split(",")[0]] = m.group(2) if m else None
            continue
        if op == "entry":
            m = ENTRY.match(args)
            if m and entry is None:
                entry = int(m.group(1), 0)
        elif op in ("addi", "addi.n", "addmi") and args.startswith("a1, a1, -"):
            m = SPSUB.match(args)
            if m:
                extra = max(extra, int(m.group(1), 0))
        elif op in ("sub", "subx2", "subx4", "subx8") and SPDYN.match(args):
            dyn = True
        elif op == "movsp" and args.startswith("a1,"):
            dyn = True
        elif op.startswith("callx"):
            m = CALLX.match(args)
            tgt = reg.get(m.group(1)) if m else None
            if tgt and tgt != name:
                if tgt not in litcalls:
                    litcalls.append(tgt)
            else:
                indirect += 1
        elif op.startswith("call"):
            m = CALL.match(f"{op} {args}")
            if m and m.group(2) != name and m.group(2) not in calls:
                calls.append(m.group(2))
        elif op.startswith("j"):
            m = JMP.match(f"{op} {args}")
            if m and m.group(1) != name and m.group(1) not in tails:
                tails.append(m.group(1))
    return {"entry": entry, "extra_sp": extra, "dyn_sp": dyn,
            "frame": (entry or 0) + extra, "calls": calls, "tails": tails,
            "litcalls": litcalls, "indirect": indirect, "insns": len(stream)}


def main():
    elf, out, anchor_file = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3])
    only = set(sys.argv[4:]) or None
    # DWARF statement addresses: always true instruction starts, and the only
    # anchors available inside a jump-table switch, whose case blocks no printed
    # branch operand points at.
    dwarf = sorted(int(x, 16) for x in anchor_file.read_text().split())
    fl = funcs_from_elf(elf)
    data, nres = {}, 0
    for i, (name, addr, size) in enumerate(fl):
        if only and name not in only:
            continue
        lo = bisect.bisect_left(dwarf, addr)
        hi = bisect.bisect_left(dwarf, addr + size)
        stream, anchors, n = decode(elf, addr, size, dwarf[lo:hi])
        nres += n - 1
        d = analyse(name, stream)
        d["addr"] = f"{addr:08x}"
        d["size"] = size
        d["resync"] = n - 1
        data[name] = d
        if i % 500 == 0:
            print(f"  {i}/{len(fl)} ...", file=sys.stderr, flush=True)
    syms = list(data)
    clone = re.compile(r"(\$(?:part|isra|constprop|cold)\$\d+)+$")
    bases = [clone.sub("", s) for s in syms]
    p = subprocess.run([CXXFILT], input="\n".join(bases), capture_output=True,
                       text=True, check=True)
    for s, b, dem in zip(syms, bases, p.stdout.splitlines()):
        data[s]["demangled"] = dem + s[len(b):]
    out.write_text(json.dumps(data))
    print(f"functions {len(data)}  extra objdump spans {nres}  "
          f"dyn_sp {sum(1 for d in data.values() if d['dyn_sp'])}")


if __name__ == "__main__":
    main()
