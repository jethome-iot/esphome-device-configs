#!/usr/bin/env python3
"""Independent Xtensa frame + edge reader over objdump -d output (no byte cols, hex imms)."""
import re, sys, pickle, bisect

S="/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack"
DIS = sys.argv[1] if len(sys.argv)>1 else S+"/firmware.dis"
OUT = sys.argv[2] if len(sys.argv)>2 else S+"/refute-dash/mine.pkl"

hdr = re.compile(r'^([0-9a-f]{8}) <(.+)>:$')
ins = re.compile(r'^([0-9a-f]{8}):\t(.*)$')
def num(s):
    s=s.strip()
    return int(s,16) if s.startswith('0x') else int(s)

funcs={}; order=[]; cur=None
for line in open(DIS):
    line=line.rstrip('\n')
    m=hdr.match(line)
    if m:
        a=int(m.group(1),16)
        cur={'addr':a,'name':m.group(2),'entry':None,'extra':0,'dyn':False,
             'calls':[],'indirect':[],'jumps':[],'n':0,'litcalls':[]}
        funcs[a]=cur; order.append(a); continue
    m=ins.match(line)
    if not m or cur is None: continue
    at=int(m.group(1),16); text=m.group(2).strip()
    cur['n']+=1
    mm=re.match(r'entry\s+a1,\s*(\S+)$',text)
    if mm:
        v=num(mm.group(1))
        if cur['entry'] is None: cur['entry']=v
        continue
    mm=re.match(r'add(?:mi|i\.n|i)\s+a1,\s*a1,\s*(-\S+)$',text)
    if mm:
        cur['extra'] += -num(mm.group(1)); continue
    if re.match(r'movsp\s+a1,',text) or re.match(r'sub\s+a1,\s*a1,',text):
        cur['dyn']=True; continue
    mm=re.match(r'call(4|8|12)\s+([0-9a-f]+)\s*(?:<(.+)>)?',text)
    if mm:
        cur['calls'].append((at,int(mm.group(2),16),mm.group(3))); continue
    mm=re.match(r'callx(4|8|12)\s+(a\d+)',text)
    if mm:
        cur['indirect'].append((at,mm.group(2))); continue
    mm=re.match(r'l32r\s+(a\d+),\s*[0-9a-f]+\s*<[^>]*>\s*\(([0-9a-f]+)\s*<(.+)>\)',text)
    if mm:
        cur['litcalls'].append((at,mm.group(1),int(mm.group(2),16),mm.group(3))); continue
    mm=re.match(r'j(?:\.l)?\s+([0-9a-f]+)\s*(?:<(.+)>)?',text)
    if mm:
        cur['jumps'].append((at,int(mm.group(1),16),mm.group(2))); continue

order.sort()
ends={}
for i,a in enumerate(order):
    ends[a]= order[i+1] if i+1<len(order) else a+0x400
def owner(addr):
    i=bisect.bisect_right(order,addr)-1
    if i<0: return None
    a=order[i]
    return a if addr<ends[a] else None

byname={}
for a,f in funcs.items():
    byname.setdefault(f['name'],[]).append(a)
    f['frame']=(f['entry'] or 0)+f['extra']
    tj=[]
    for at,tgt,nm in f['jumps']:
        o=owner(tgt)
        if o is not None and o!=a: tj.append((at,tgt,funcs[o]['name']))
    f['tailjumps']=tj
    # resolve literal-pool indirect calls: l32r aN,<sym> ... callx8 aN
    res=[]
    lit={}
    for at,reg,tgt,nm in f['litcalls']: lit.setdefault(reg,[]).append((at,tgt,nm))
    for at,reg in f['indirect']:
        best=None
        for lat,tgt,nm in lit.get(reg,[]):
            if lat<at and (best is None or lat>best[0]): best=(lat,tgt,nm)
        if best and owner(best[1])==best[1]:
            res.append((at,best[1],best[2]))
    f['litresolved']=res

pickle.dump({'funcs':funcs,'byname':byname,'order':order,'ends':ends,'owner_order':order},open(OUT,'wb'))
print("functions:",len(funcs),"with entry:",sum(1 for f in funcs.values() if f['entry'] is not None),
      "dyn:",sum(1 for f in funcs.values() if f['dyn']))
