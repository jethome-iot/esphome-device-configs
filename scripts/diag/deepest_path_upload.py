#!/usr/bin/env python3
"""Deepest-stack walk over the archive-derived graph + explicit indirect seeds.
SCCs are condensed so any recursion is reported, never silently summed."""
import json, sys
from pathlib import Path
BASE = Path(__file__).parent
O = json.loads((BASE/"objgraph.json").read_text())
G = json.loads((BASE/"graph.json").read_text())          # linked-ELF fallback
SEEDS = json.loads((BASE/"seeds.json").read_text())
CUT = set(json.loads((BASE/"cut.json").read_text())) if (BASE/"cut.json").exists() else set()

def frame(s):
    if s in O: return O[s]['frame']
    if s in G: return G[s]['frame']
    return 0
def dem(s):
    if s in O: return O[s].get('demangled', s)
    if s in G: return G[s]['demangled']
    return s
def succ(s):
    out = []
    if s in O: out += O[s]['calls']
    elif s in G: out += G[s]['calls']
    out += SEEDS.get(s, [])
    seen=set(); r=[]
    for e in out:
        if e in CUT or e in seen: continue
        seen.add(e)
        if e in G: r.append(e)   # must exist in the LINKED image
    return r

def reach(root):
    seen=set(); st=[root]
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

def run(root, topn=1):
    nodes=reach(root); comps=tarjan(nodes)
    cid={s:i for i,c in enumerate(comps) for s in c}
    selfl={i for i,c in enumerate(comps) if len(c)==1 and c[0] in succ(c[0])}
    dag={i:set() for i in range(len(comps))}
    for s in nodes:
        for t in succ(s):
            if t in cid and cid[t]!=cid[s]: dag[cid[s]].add(cid[t])
    def cost(i):
        c=comps[i]
        return sum(frame(x) for x in c)
    memo={}
    def best(i):
        if i in memo: return memo[i]
        b,bp=0,[]
        for j in dag[i]:
            v,p=best(j)
            if v>b: b,bp=v,p
        memo[i]=(cost(i)+b, [i]+bp); return memo[i]
    tot,path=best(cid[root])
    print(f"\n########## ROOT {dem(root)[:110]}")
    print(f"           reachable {len(nodes)} symbols, deepest chain {tot} bytes")
    run_=0
    for i in path:
        c=comps[i]; run_+=cost(i)
        if len(c)==1 and i not in selfl:
            print(f"   {frame(c[0]):5d} {run_:6d}  {dem(c[0])[:115]}")
        else:
            print(f"   {cost(i):5d} {run_:6d}  [RECURSIVE SCC n={len(c)}] " + " | ".join(dem(x)[:45] for x in c))
    nt=[i for i,c in enumerate(comps) if len(c)>1 or i in selfl]
    if nt:
        print("   --- recursive components anywhere in the reachable set:")
        for i in nt:
            print(f"       n={len(comps[i])} level={cost(i)}: " + " | ".join(dem(x)[:60] for x in comps[i]))
    else:
        print("   --- no recursion anywhere in the reachable set")
    return tot,path,comps,cid

if __name__=="__main__":
    for r in sys.argv[1:]: run(r)

def branches(root):
    """cost of each callee subtree of `root`, ranked"""
    nodes=reach(root); comps=tarjan(nodes)
    cid={s:i for i,c in enumerate(comps) for s in c}
    dag={i:set() for i in range(len(comps))}
    for s in nodes:
        for t in succ(s):
            if t in cid and cid[t]!=cid[s]: dag[cid[s]].add(cid[t])
    def cost(i): return sum(frame(x) for x in comps[i])
    memo={}
    def best(i):
        if i in memo: return memo[i]
        b,bp=0,[]
        for j in dag[i]:
            v,p=best(j)
            if v>b: b,bp=v,p
        memo[i]=(cost(i)+b,[i]+bp); return memo[i]
    print(f"\n=== branches of {dem(root)[:100]}  (own frame {frame(root)})")
    rows=[]
    for c in succ(root):
        if c in cid:
            v,p=best(cid[c]); rows.append((v,c,p))
    for v,c,p in sorted(rows, reverse=True)[:14]:
        chain=" -> ".join(dem(comps[i][0])[:34] for i in p[:9])
        print(f"  {v:6d}  {dem(c)[:70]}\n            {chain}")
