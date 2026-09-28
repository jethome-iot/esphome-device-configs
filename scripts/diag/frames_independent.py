import re,sys,json,subprocess
DIS=sys.argv[1]
sym_re=re.compile(r'^([0-9a-f]{8}) <(.+)>:$')
ins_re=re.compile(r'^([0-9a-f]{8}):\t(\S+)\s*(.*)$')
funcs={}   # addr -> dict(name, frame, extra, dynamic, calls=set of addr, tails=set)
cur=None
lit={}     # literal addr -> value (from "l32r aN, <addr> <sym> (val <...>)")
l32r_re=re.compile(r'l32r\s+(a\d+), ([0-9a-f]+) <[^>]*> \(([0-9a-f]+) ')
for line in open(DIS,errors='replace'):
    line=line.rstrip('\n')
    m=sym_re.match(line)
    if m:
        a=int(m.group(1),16)
        cur={'addr':a,'name':m.group(2),'frame':None,'extra':0,'dyn':False,'calls':set(),'tails':set(),'regs':{}}
        funcs[a]=cur
        continue
    if cur is None: continue
    m=ins_re.match(line)
    if not m: continue
    op=m.group(2); args=m.group(3)
    if op=='entry':
        mm=re.match(r'a1,\s*(0x[0-9a-fA-F]+|\d+)',args)
        if mm and cur['frame'] is None: cur['frame']=int(mm.group(1),0)
    elif op in ('addi','addmi','addi.n'):
        mm=re.match(r'a1,\s*a1,\s*(-?(?:0x[0-9a-fA-F]+|\d+))',args)
        if mm:
            v=int(mm.group(1),0)
            if v<0: cur['extra']+= -v
    elif op=='movsp':
        cur['dyn']=True
    elif op=='sub':
        if re.match(r'a1,\s*a1',args): cur['dyn']=True
    elif op=='l32r':
        mm=l32r_re.search(line)
        if mm: cur['regs'][mm.group(1)]=int(mm.group(3),16)
    elif op in ('call8','call4','call12','call0'):
        mm=re.match(r'([0-9a-f]+) <',args)
        if mm: cur['calls'].add(int(mm.group(1),16))
    elif op in ('callx8','callx4','callx12','callx0'):
        mm=re.match(r'(a\d+)',args)
        if mm and mm.group(1) in cur['regs']: cur['calls'].add(cur['regs'][mm.group(1)])
        else: cur['calls'].add(-1)
    elif op in ('j','j.l'):
        mm=re.match(r'([0-9a-f]+) <([^>+]+)>$',args)
        if mm:
            t=int(mm.group(1),16)
            if t in funcs and t!=cur['addr']: cur['tails'].add(t)
    elif op in ('jx',):
        pass
out={}
for a,f in funcs.items():
    out[hex(a)]={'name':f['name'],'frame':(f['frame'] or 0)+f['extra'],'entry':f['frame'],'extra':f['extra'],
                 'dyn':f['dyn'],'calls':[hex(x) for x in sorted(f['calls']) if x>=0],
                 'indirect':(-1 in f['calls']),'tails':[hex(x) for x in sorted(f['tails'])]}
json.dump(out,open(sys.argv[2],'w'))
print(len(out),'functions')
