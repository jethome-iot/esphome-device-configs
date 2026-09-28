import re,sys,json,collections,bisect
dis='fw.dis'
hdr=re.compile(r'^([0-9a-f]{8}) <(.+)>:')
ins=re.compile(r'^\s*([0-9a-f]+):\t(?:[0-9a-f]{2,8}\s+)\t*(\S+)\t?(.*)$')
l32r=re.compile(r'^a(\d+),\s*\S+\s*<[^>]*>\s*\(([0-9a-f]+)\s*<([^>]+)>\)')
frames={}; extra=collections.defaultdict(int); dyn=set()
direct=collections.defaultdict(set); lit=collections.defaultdict(set); unres=collections.defaultdict(int)
addr_of={}; name_at={}
sym=None; regs={}
lines=open(dis,errors='replace').read().split('\n')
for line in lines:
    m=hdr.match(line)
    if m:
        sym=m.group(2); a=int(m.group(1),16); addr_of[sym]=a; name_at[a]=sym
        frames.setdefault(sym,0); regs={}; continue
    if sym is None: continue
    m=ins.match(line)
    if not m: continue
    op=m.group(2); raw=m.group(3); args=raw.replace(' ','').split('<')[0].rstrip()
    if op=='entry':
        p=args.split(',')
        if len(p)==2 and frames.get(sym,0)==0:
            try: frames[sym]=int(p[1],0)
            except: pass
    elif op in ('addi','addmi','addi.n') and args.startswith('a1,a1,-'):
        extra[sym]+=-int(args.split(',')[2])
    elif op=='movsp' and args.startswith('a1,'): dyn.add(sym)
    elif op=='sub' and args.startswith('a1,a1'): dyn.add(sym)
    elif op=='l32r':
        mm=l32r.match(raw.strip())
        if mm: regs['a'+mm.group(1)]=int(mm.group(2),16)
        else:
            r=raw.strip().split(',')[0]
            regs.pop(r,None)
    elif op in ('mov','mov.n','l32i','l32i.n','movi','movi.n','add','add.n','addi','addi.n','addmi','sub','l8ui','l16ui','l16si','s32i','s32i.n'):
        r=raw.strip().split(',')[0]
        if op not in ('s32i','s32i.n'): regs.pop(r,None)
    elif op in ('call0','call4','call8','call12'):
        if re.match(r'^[0-9a-f]{6,8}$',args): direct[sym].add(int(args,16))
    elif op in ('callx0','callx4','callx8','callx12'):
        r=args.strip()
        if r in regs: lit[sym].add(regs[r])
        else: unres[sym]+=1
starts=sorted(name_at)
def owner(a):
    i=bisect.bisect_right(starts,a)-1
    return name_at[starts[i]] if i>=0 else None
out={}
for k in frames:
    s=set()
    for a in direct.get(k,()):
        t=owner(a)
        if t and t!=k or (t==k): s.add(t)
    li=set()
    for a in lit.get(k,()):
        t=owner(a)
        if t: li.add(t)
    out[k]={'d':sorted(x for x in s if x),'i':sorted(li),'u':unres.get(k,0)}
json.dump({'frames':frames,'extra':dict(extra),'dyn':sorted(dyn),'addr':addr_of,'g':out},open('g2.json','w'))
print(len(frames),'syms',len(dyn),'dyn', sum(v['u'] for v in out.values()),'unresolved callx')
