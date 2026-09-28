#!/usr/bin/env python3
"""Find every self-recursive call in the build's archives.

archive_graph_upload.py cannot see these: a same-section direct call disassembles
as `call8 <name+0xNN>`, which its `<([^>+])+>` callee regex rejects, and the
R_XTENSA_SLOT0_OP relocation that names the target is collected into `_slot0` and
then dropped.  With -ffunction-sections a same-section direct call is always a
self-call, so this is exactly the set of recursion edges that graph is missing.

usage: selfcall_scan.py <objdump -dr output of the archives>
"""
import re, subprocess, sys
CXXFILT = ("/home/alexey/.cache/esphome/idf/tools/xtensa-esp-elf/"
           "esp-14.2.0_20260121/xtensa-esp-elf/bin/xtensa-esp32-elf-c++filt")
SYM = re.compile(r"^[0-9a-f]+ <(.+)>:$")
REL = re.compile(r"^\s+([0-9a-f]+): (R_XTENSA_\S+)\s+(\S+)")

def scan(path):
    cur, hits = None, {}
    with open(path, errors="replace") as fh:
        for line in fh:
            m = SYM.match(line.rstrip("\n"))
            if m:
                cur = m.group(1)
                continue
            if cur is None:
                continue
            m = REL.match(line)
            if m and m.group(3) == cur and m.group(2) in (
                    "R_XTENSA_SLOT0_OP", "R_XTENSA_ASM_EXPAND"):
                hits.setdefault(cur, set()).add(int(m.group(1), 16))
    return hits

if __name__ == "__main__":
    hits = scan(sys.argv[1])
    names = sorted(hits)
    dem = subprocess.run([CXXFILT], input="\n".join(names),
                         capture_output=True, text=True).stdout.splitlines()
    for n, d in zip(names, dem):
        print(f"{len(hits[n]):3d} self-call(s)  {d}")
    print(f"\n{len(names)} self-recursive functions")
