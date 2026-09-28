import json,subprocess,sys,collections
d=json.load(open('g2.json')); F=d['frames']; E=d['extra']; G=d['g']
names=list(F)
dem=dict(zip(names,subprocess.run(['c++filt'],input='\n'.join(names),capture_output=True,text=True).stdout.split('\n')))
SEEDS=json.load(open('seeds.json'))
CUT=set(json.load(open('cut.json')))
ALLOW=tuple(json.load(open('allow.json')))
adj=collections.defaultdict(set)
for k in names:
    for t in set(G[k]['d']+G[k]['i']):
        if t in F: adj[k].add(t)
for k,v in SEEDS.items():
    for t in v:
        if t in F: adj[k].add(t)
def ok(n):
    if n in CUT: return False
    return n.startswith(ALLOW)
root=sys.argv[1]
# induced reachable set
seen=set(); st=[root]; boundary=collections.Counter()
while st:
    v=st.pop()
    if v in seen: continue
    seen.add(v)
    for t in adj[v]:
        if ok(t):
            if t not in seen: st.append(t)
        else: boundary[t]+=1
def cost(n): return F.get(n,0)+E.get(n,0)
sub={v:{t for t in adj[v] if t in seen} for v in seen}
# Tarjan
idx={};low={};on={};stk=[];comp={};comps=[];ctr=[0]
def scc(root):
    work=[(root,iter(sub[root]))]; idx[root]=low[root]=ctr[0];ctr[0]+=1;stk.append(root);on[root]=1
    while work:
        v,it=work[-1]; adv=False
        for w in it:
            if w not in idx:
                idx[w]=low[w]=ctr[0];ctr[0]+=1;stk.append(w);on[w]=1;work.append((w,iter(sub[w])));adv=True;break
            elif on.get(w): low[v]=min(low[v],idx[w])
        if adv: continue
        work.pop()
        if work: u=work[-1][0]; low[u]=min(low[u],low[v])
        if low[v]==idx[v]:
            c=[]
            while True:
                w=stk.pop(); on[w]=0; comp[w]=len(comps); c.append(w)
                if w==v: break
            comps.append(c)
for n in sub:
    if n not in idx: scc(n)
memo={}
def best(ci):
    if ci in memo: return memo[ci]
    memo[ci]=(0,[])
    c=comps[ci]; w=sum(cost(x) for x in c); bs=(0,[])
    for x in c:
        for t in sub[x]:
            if comp[t]!=ci:
                r=best(comp[t])
                if r[0]>bs[0]: bs=r
    memo[ci]=(w+bs[0],[('SCC',c) if len(c)>1 else c[0]]+bs[1]); return memo[ci]
r=best(comp[root])
print(f"### nodes={len(seen)}  deepest from {root} = {r[0]}")
tot=0
for e in r[1]:
    if isinstance(e,tuple):
        print(f"   [CYCLE {len(e[1])}, sum {sum(cost(x) for x in e[1])}]: "+", ".join(dem.get(x,x)[:50] for x in e[1][:8]))
    else:
        tot+=cost(e); print(f"  {cost(e):5d} {tot:6d}  {dem.get(e,e)[:95]}")
print("--- boundary cuts (top 25):")
for n,c in boundary.most_common(25): print(f"   {F.get(n,'?'):>5} {dem.get(n,n)[:85]}")
