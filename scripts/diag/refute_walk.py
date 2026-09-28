#!/usr/bin/env python3
"""Independent deepest-stack walk. Frames from the archives (objgraph) with the
linked ELF as fallback; SCCs condensed so recursion is never silently summed."""
import json, sys, collections
from pathlib import Path
BASE = Path("/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack")
O = json.loads((BASE/"slice-upload/objgraph.json").read_text())
G = json.loads((BASE/"slice-upload/graph.json").read_text())
SEEDS = json.loads((BASE/"rd/seeds.json").read_text())

# only the panic/abort tail is cut: once abort() runs the device is already down.
CUT = {"__assert_func","abort","_raise_r","raise","_exit","esp_system_abort",
       "panic_abort","esp_rom_printf","esp_rom_vprintf"}
EXTRA_CUT = set(json.loads(Path(sys.argv[1]).read_text())) if len(sys.argv)>1 and sys.argv[1]!='-' else set()
CUT |= EXTRA_CUT

def frame(s):
    if s in O and O[s]['frame']: return O[s]['frame']
    if s in G: return G[s]['frame']
    return O.get(s,{}).get('frame',0)
def dem(s):
    return (O.get(s,{}).get('demangled') or G.get(s,{}).get('demangled') or s)
_sc={}
def succ(s):
    if s in _sc: return _sc[s]
    out = list(O[s]['calls']) if s in O else (list(G[s]['calls'])+list(G[s].get('tails',[])) if s in G else [])
    out += SEEDS.get(s, [])
    seen=set(); r=[]
    for e in out:
        if e in CUT or e in seen: continue
        seen.add(e)
        if e in G or e in O: r.append(e)
    _sc[s]=r; return r

def reach(roots):
    seen=set(); st=list(roots)
    while st:
        s=st.pop()
        if s in seen: continue
        seen.add(s); st.extend(succ(s))
    return seen

def tarjan(nodes):
    idx={}; low={}; on={}; stk=[]; comps=[]; c=[0]
    for v in nodes:
        if v in idx: continue
        work=[(v, iter(succ(v)))]; idx[v]=low[v]=c[0]; c[0]+=1; stk.append(v); on[v]=True
        while work:
            u,it=work[-1]; adv=False
            for w in it:
                if w not in nodes: continue
                if w not in idx:
                    idx[w]=low[w]=c[0]; c[0]+=1; stk.append(w); on[w]=True
                    work.append((w, iter(succ(w)))); adv=True; break
                elif on.get(w): low[u]=min(low[u], idx[w])
            if adv: continue
            work.pop()
            if work: low[work[-1][0]]=min(low[work[-1][0]], low[u])
            if low[u]==idx[u]:
                comp=[]
                while True:
                    w=stk.pop(); on[w]=False; comp.append(w)
                    if w==u: break
                comps.append(comp)
    return comps

def analyse(root, label, show=60):
    nodes=reach([root]); comps=tarjan(nodes)
    cid={s:i for i,c in enumerate(comps) for s in c}
    selfl={i for i,c in enumerate(comps) if len(c)==1 and c[0] in succ(c[0])}
    dag=collections.defaultdict(set)
    for s in nodes:
        for t in succ(s):
            if t in cid and cid[t]!=cid[s]: dag[cid[s]].add(cid[t])
    cost=lambda i: sum(frame(x) for x in comps[i])
    memo={}
    def best(i):
        if i in memo: return memo[i]
        memo[i]=(0,[i])  # guard
        b,bp=0,[]
        for j in dag[i]:
            v,p=best(j)
            if v>b: b,bp=v,p
        memo[i]=(cost(i)+b,[i]+bp); return memo[i]
    tot,path=best(cid[root])
    print(f"\n########## {label}: {dem(root)[:100]}")
    print(f"  reachable {len(nodes)} symbols, deepest chain {tot} bytes")
    run=0
    for i in path[:show]:
        run+=cost(i); c=comps[i]
        tag = "" if (len(c)==1 and i not in selfl) else f"[SCC n={len(c)}] "
        print(f"   {cost(i):5d} {run:6d}  {tag}{' | '.join(dem(x)[:90] for x in c)}")
    nt=[i for i,c in enumerate(comps) if len(c)>1 or i in selfl]
    print(f"  recursive components in closure: {len(nt)}")
    for i in nt[:20]:
        print(f"     n={len(comps[i])} level={cost(i)}: " + " | ".join(dem(x)[:55] for x in comps[i]))
    return tot
if __name__=="__main__":
    GET="_ZN7esphome14web_server_idf14AsyncWebServer15request_handlerEP9httpd_req"
    POST="_ZN7esphome14web_server_idf14AsyncWebServer20request_post_handlerEP9httpd_req"
    PRE=["httpd_thread","httpd_sess_enum","httpd_process_session","httpd_sess_process","httpd_req_new","httpd_uri"]
    pre=sum(frame(s) for s in PRE)
    print("httpd prologue (thread..httpd_uri):", pre, [ (s,frame(s)) for s in PRE])
    for lbl,r in (("GET root",GET),("POST root",POST)):
        t=analyse(r,lbl)
        print(f"  ==> {lbl} TOTAL incl. prologue {pre}: {pre+t}")
