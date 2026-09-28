#!/usr/bin/env python3
"""Call graph from an objdump -d listing that also resolves literal-pool calls
(`l32r aN, <lit> (<addr> <sym>)` followed by `callx8 aN`), which the
a plain call/callx parser counts as unresolved indirect calls."""
import json, os, re, subprocess, sys
from pathlib import Path

DIS = Path(sys.argv[1])          # objdump -d of the ELF with .xt.prop stripped
OUT = Path(sys.argv[2])          # where to write the JSON graph
CXXFILT = os.environ.get("CXXFILT", "xtensa-esp32-elf-c++filt")

FUNC  = re.compile(r"^([0-9a-f]{8}) <(.+)>:$")
INSN  = re.compile(r"^\s*[0-9a-f]+:\t(\S+)\s*(.*)$")
CALL  = re.compile(r"^(call0|call4|call8|call12)\s+[0-9a-f]+ <([^>+]+)>")
JMP   = re.compile(r"^j(?:\.l)?\s+[0-9a-f]+ <([^>+]+)>$")
L32R  = re.compile(r"^(a\d+),\s+[0-9a-f]+ <[^>]*> \(([0-9a-f]+) <([^>]+)>\)$")
MOV   = re.compile(r"^(a\d+),\s*(a\d+)$")
NUM   = r"(0x[0-9a-fA-F]+|\d+)"
ENTRY = re.compile(rf"^a1,\s*{NUM}$")
SPSUB = re.compile(rf"^a1,\s*a1,\s*-{NUM}$")
SPDYN = re.compile(r"^a1,\s*a1,\s*a\d+$")

funcs, cur, regs = {}, None, {}
for line in DIS.open(encoding='utf-8', errors='replace'):
    line = line.rstrip('\n')
    m = FUNC.match(line)
    if m:
        cur = dict(addr=m.group(1), sym=m.group(2), entry=None, extra_sp=0, dyn_sp=False,
                   calls=set(), lit_calls=set(), tails=set(), lit_tails=set(),
                   indirect=0, insns=0)
        funcs[m.group(2)] = cur
        regs = {}
        continue
    if cur is None:
        continue
    im = INSN.match(line)
    if not im:
        continue
    op, args = im.group(1), im.group(2).strip()
    cur['insns'] += 1
    if op == 'entry':
        em = ENTRY.match(args)
        if em and cur['entry'] is None: cur['entry'] = int(em.group(1), 0)
    elif op in ('addi','addi.n','addmi') and args.startswith('a1, a1, -'):
        sm = SPSUB.match(args)
        if sm: cur['extra_sp'] = max(cur['extra_sp'], int(sm.group(1), 0))
    elif op in ('sub','subx2','subx4','subx8') and SPDYN.match(args):
        cur['dyn_sp'] = True
    elif op == 'movsp' and args.startswith('a1,'):
        cur['dyn_sp'] = True
    elif op == 'l32r':
        lm = L32R.match(args)
        if lm: regs[lm.group(1)] = (lm.group(2), lm.group(3))
        else:
            rm = re.match(r"^(a\d+),", args)
            if rm: regs.pop(rm.group(1), None)
    elif op in ('mov','mov.n','or') and MOV.match(args):
        d, s = MOV.match(args).groups()
        if s in regs: regs[d] = regs[s]
        else: regs.pop(d, None)
    elif op.startswith('callx'):
        rm = re.match(r"^(a\d+)$", args)
        tgt = regs.get(rm.group(1)) if rm else None
        if tgt: cur['lit_calls'].add(tgt)
        else: cur['indirect'] += 1
    elif op.startswith('call'):
        cm = CALL.match(f"{op} {args}")
        if cm: cur['calls'].add(cm.group(2))
    elif op == 'jx':
        cur['indirect'] += 1
    elif op.startswith('j'):
        jm = JMP.match(f"{op} {args}")
        if jm and jm.group(1) != cur['sym']: cur['tails'].add(jm.group(1))
    # a register written by anything else becomes unknown
    if op not in ('l32r','mov','mov.n','or') and not op.startswith(('call','j','b','ret')):
        wm = re.match(r"^(a\d+),", args)
        if wm: regs.pop(wm.group(1), None)

# map address -> symbol name for literal targets
by_addr = {f['addr']: s for s, f in funcs.items()}
for s, f in funcs.items():
    lit = set()
    for addr, sym in f['lit_calls']:
        name = by_addr.get(addr)
        if name: lit.add(name)
        elif sym in funcs: lit.add(sym)
    f['lit_resolved'] = lit

CLONE = re.compile(r"(\$(?:part|isra|constprop|cold)\$\d+)+$")
syms = list(funcs); bases = [CLONE.sub('', s) for s in syms]
p = subprocess.run([CXXFILT], input='\n'.join(bases), capture_output=True, text=True, check=True)
dem = {s: d + s[len(b):] for s, b, d in zip(syms, bases, p.stdout.splitlines())}
data = {}
for s, f in funcs.items():
    data[s] = dict(addr=f['addr'], demangled=dem.get(s, s),
                   frame=(f['entry'] or 0) + f['extra_sp'], entry_imm=f['entry'],
                   extra_sp=f['extra_sp'], dyn_sp=f['dyn_sp'], indirect=f['indirect'],
                   calls=sorted(f['calls']), lit=sorted(f['lit_resolved']),
                   tails=sorted(f['tails']), insns=f['insns'])
OUT.write_text(json.dumps(data))
nlit = sum(len(v['lit']) for v in data.values())
print(f"functions {len(data)}  direct edges {sum(len(v['calls']) for v in data.values())}"
      f"  literal-pool edges {nlit}  still-unresolved indirect {sum(v['indirect'] for v in data.values())}")
