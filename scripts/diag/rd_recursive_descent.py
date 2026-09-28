#!/usr/bin/env python3
"""Recursive-descent Xtensa decode: exact instruction lengths from the ELF bytes,
instruction text from objdump (resynced where the linear decode desynced).

Length rule: 2 bytes iff (byte0 & 0x0F) in {8,9,0xC,0xD} (density formats),
else 3 bytes. GAS leaves 1-byte fill in unreachable slots (after retw.n, before
an aligned branch target), which desyncs any linear decode; walking from `entry`
and following every branch target avoids it.
"""
import re, os, json, subprocess, sys, glob

SCR = "/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack"
ELF = SCR + "/firmware.noprop.elf"
DIS = SCR + "/firmware.dis"
OBJDUMP = glob.glob("/home/alexey/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32-elf-objdump")[0]

HDR = re.compile(r'^([0-9a-f]{8}) <(.+)>:$')
INSN = re.compile(r'^([0-9a-f]{8}):\t(?:[0-9a-f]{2} )*\s*(\S+)(?:\s+(.*))?$')
BRTGT = re.compile(r'\b([0-9a-f]{8}) <')
UNCOND = {'j', 'j.l', 'ret', 'ret.n', 'retw', 'retw.n', 'jx', 'ill', 'callx0'}
# ops that do not write their first register operand
NO_WRITE_RE = re.compile(r'^(b|j|jx|ret|retw|loop|nop|memw|isync|rsync|dsync|esync|extw|excw|waiti'
                         r'|call|callx|s8i|s16i|s32i|s32c1i|s32ri|ssa8|ssl|ssr|ssai|wsr|wur|entry'
                         r'|movsp|ill|rfi|rfe|rfwo|rfwu|wdtlb|witlb|idtlb|iitlb|dhwb|dhi|ihi|iii'
                         r'|dpfr|dpfw|dpfro|dpfwo|ipf|wfr?|st|mov\.s|ssi|ssiu|ssx|ssxu)')


def parse_stream(lines):
    insns, heads = {}, {}
    for line in lines:
        line = line.rstrip('\n')
        m = HDR.match(line)
        if m:
            heads[int(m.group(1), 16)] = m.group(2)
            continue
        mi = INSN.match(line)
        if mi:
            insns[int(mi.group(1), 16)] = (mi.group(2), mi.group(3) or '')
    return insns, heads


class Image:
    """Byte view of the executable sections."""
    def __init__(self):
        self.segs = []
        out = subprocess.run(['readelf', '-S', '-W', ELF], capture_output=True, text=True).stdout
        raw = open(ELF, 'rb').read()
        for line in out.splitlines():
            m = re.match(r'\s*\[\s*\d+\]\s+(\S+)\s+(PROGBITS)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)', line)
            if not m:
                continue
            name, _, addr, off, size = m.groups()
            addr, off, size = int(addr, 16), int(off, 16), int(size, 16)
            if size and ('text' in name or 'vectors' in name):
                self.segs.append((addr, addr + size, raw[off:off + size]))

    def byte(self, a):
        for lo, hi, b in self.segs:
            if lo <= a < hi:
                return b[a - lo]
        return None

    def ilen(self, a):
        b = self.byte(a)
        if b is None:
            return None
        return 2 if 0x8 <= (b & 0x0F) <= 0xD else 3


class Decoder:
    def __init__(self):
        with open(DIS, errors='replace') as fh:
            self.lin, self.heads = parse_stream(fh)
        self.img = Image()
        self.extra = {}
        ha = sorted(self.heads)
        self.fn_end = {a: (ha[i + 1] if i + 1 < len(ha) else a + 0x2000) for i, a in enumerate(ha)}
        self.name_addr = {}
        for a, n in self.heads.items():
            self.name_addr.setdefault(n, a)
        self.addr_name = dict(self.heads)
        self._done = set()
        self._cache = {}

    def at(self, a):
        return self.extra.get(a) or self.lin.get(a)

    def resync(self, a, end):
        if (a, end) in self._done:
            return False
        self._done.add((a, end))
        out = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', f'--start-address=0x{a:08x}',
                              f'--stop-address=0x{end:08x}', ELF],
                             capture_output=True, text=True, errors='replace')
        ins, _ = parse_stream(out.stdout.splitlines())
        grew = False
        for k, v in ins.items():
            if k not in self.extra:
                self.extra[k] = v
                grew = True
        return grew

    def walk(self, fname):
        if fname in self._cache:
            return self._cache[fname]
        start = self.name_addr.get(fname)
        if start is None:
            return None
        end = self.fn_end[start]
        r = None
        for _ in range(16):
            r = self._once(start, end)
            if not r['orphans']:
                break
            grew = False
            for o in sorted(r['orphans']):
                if self.resync(o, end):
                    grew = True
            if not grew:
                break
        self._cache[fname] = r
        return r

    def _once(self, start, end):
        seen, orphans = {}, set()
        work = [start]
        entry = None
        extra_sp = 0
        dyn = False
        calls, tails, bogus = [], [], []
        indirect = 0
        lits = {}
        while work:
            a = work.pop()
            if a in seen or not (start <= a < end):
                continue
            iv = self.at(a)
            if iv is None:
                orphans.add(a)
                continue
            seen[a] = iv
            op, args = iv
            ln = self.img.ilen(a) or 3
            if op == 'entry' and entry is None and a == start:
                mm = re.match(r'a1,\s*(0x[0-9a-fA-F]+|\d+)', args)
                if mm:
                    entry = int(mm.group(1), 0)
            elif op in ('addi', 'addmi', 'addi.n') and args.startswith('a1, a1,'):
                tok = args.split(',')[2].strip()
                try:
                    n = int(tok, 0)
                except ValueError:
                    dyn = True
                    n = 0
                if n >= 0x80000000:
                    n -= 0x100000000
                if n < 0:
                    extra_sp += -n
            elif op == 'movsp' and args.startswith('a1'):
                dyn = True
            elif op == 'sub' and args.startswith('a1, a1'):
                dyn = True
            elif op == 'l32r':
                mm = re.match(r'(a\d+),\s*[0-9a-f]+ <[^>]*> \(([0-9a-f]+) <([^>]+)>\)', args)
                if mm:
                    lits[mm.group(1)] = mm.group(3)
                else:
                    m2 = re.match(r'(a\d+),', args)
                    if m2:
                        lits.pop(m2.group(1), None)
            elif op in ('mov', 'mov.n', 'or') and re.match(r'a\d+,\s*a\d+', args):
                d, s = re.match(r'(a\d+),\s*(a\d+)', args).groups()
                if s in lits:
                    lits[d] = lits[s]
                else:
                    lits.pop(d, None)
            elif op.startswith('callx'):
                mm = re.match(r'(a\d+)', args)
                t = lits.get(mm.group(1)) if mm else None
                if t:
                    if '+' in t:
                        bogus.append((a, op, t))
                    else:
                        calls.append(t)
                else:
                    indirect += 1
            elif op.startswith('call') and not op.startswith('callx'):
                mm = re.search(r'<([^>]+)>', args)
                if mm:
                    if '+' in mm.group(1):
                        bogus.append((a, op, args))
                    else:
                        calls.append(mm.group(1))
            else:
                mm = re.match(r'(a\d+)\b', args)
                if mm and not NO_WRITE_RE.match(op):
                    lits.pop(mm.group(1), None)
            tgts = [int(m.group(1), 16) for m in BRTGT.finditer(args)]
            for t in tgts:
                if start <= t < end:
                    work.append(t)
                elif op in ('j', 'jx') and t in self.addr_name:
                    tails.append(self.addr_name[t])
            if op not in UNCOND:
                work.append(a + ln)
        return dict(frame=(entry or 0) + extra_sp, entry=entry, extra=extra_sp, dyn=dyn,
                    calls=sorted(set(calls)), tails=sorted(set(tails)), indirect=indirect,
                    orphans=orphans, n=len(seen), start=start, end=end, bogus=bogus)


if __name__ == '__main__':
    d = Decoder()
    for nm in sys.argv[1:]:
        for c in [n for n in d.name_addr if nm in n]:
            r = d.walk(c)
            print(f"{c}\n  {d.name_addr[c]:08x} frame {r['frame']} entry {r['entry']} extra {r['extra']} "
                  f"dyn {r['dyn']} indirect {r['indirect']} insns {r['n']} orphans {[hex(o) for o in sorted(r['orphans'])]} tails {r['tails']}")
            for x in r['calls']:
                print('   ->', x)
