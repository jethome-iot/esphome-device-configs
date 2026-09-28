import json,sys
g=json.load(open('g.json'))
EXCL_SUB=['__assert_func','abort','panic','esp_system_abort','__cxa_throw','_Unwind','__gxx',
          'esp_restart','vTaskDelete','__cxa_pure_virtual','std::terminate','esp_backtrace']
CUT=set(sys.argv[2].split(',')) if len(sys.argv)>2 and sys.argv[2] else set()
def excluded(a):
    d=g[a].get('dem',g[a]['name'])
    return any(e in d for e in EXCL_SUB)
def succ(a):
    return [c for c in (g[a]['calls']+g[a]['tails']) if c in g and not excluded(c) and c not in CUT]
sys.setrecursionlimit(200000)
def dfs(a,path,cache):
    if a in cache: return cache[a]
    best=0;bestp=[];clean=True
    for c in succ(a):
        if c in path: clean=False;continue
        d,p,cl=dfs(c,path|{c},cache)
        clean=clean and cl
        if d>best: best,bestp=d,p
    res=(g[a]['frame']+best,[a]+bestp,clean)
    if clean: cache[a]=res
    return res
cache={}
start=sys.argv[1]
d,p,cl=dfs(start,{start},cache)
print(f"total {d} clean={cl} depth={len(p)}")
for a in p: print(f"   {g[a]['frame']:5d}  {a} {g[a].get('dem',g[a]['name'])[:100]}")
