#!/usr/bin/env python3
"""Deepest-path walker over my own frame/edge graph."""
import pickle,re,sys,subprocess,os,json
S="/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack"
D=pickle.load(open(S+"/refute-dash/mine.pkl","rb"))
F=D['funcs']
CXXFILT="/home/alexey/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp-elf-c++filt"
if not os.path.exists(CXXFILT): CXXFILT="c++filt"
names=[f['name'] for f in F.values()]
p=subprocess.run([CXXFILT],input="\n".join(names),capture_output=True,text=True)
dm=dict(zip(names,p.stdout.strip("\n").split("\n")))
for f in F.values(): f['dn']=dm.get(f['name'],f['name'])

# cut set: functions whose callees do NOT execute on the httpd task, or that terminate the program
CUT_RE = [
 r'^__assert_func$', r'^abort$', r'^esp_system_abort$', r'^panic_abort$', r'^_esp_error_check_failed',
 r'^vTaskDelete', r'^esp_restart', r'^vPortYield$',
]
CUTS=set()
for a,f in F.items():
    for r in CUT_RE:
        if re.search(r, f['name']): CUTS.add(a)

# proven-dead edges (caller-side guard verified in the disassembly)
EDGE_CUTS = {
  ("esp_cache_get_alignment","esp_log_timestamp"),   # ESP_RETURN_ON_FALSE(out!=NULL); every caller passes a stack addr
  ("__ssprint_r","_realloc_r"),                      # needs fp->_flags & (__SMBF|__SOPT): asprintf only
  ("__ssprint_r","_malloc_r"),
  ("_svfprintf_r","_malloc_r"),                      # bbci a8,7 -> needs __SMBF (asprintf); snprintf sets __SWR|__SSTR
  ("__ssprint_r","memcpy"),
  ("xPortEnterCriticalTimeout","__assert_func"),     # fatal assert leaf, not a silent-corruption path
  ("cache_hal_get_cache_line_size","__assert_func"),
}
def edges(a, use_lit=True):
    f=F[a]; out=[]
    for at,tgt,nm in f['calls']:
        if tgt in F and (f['name'],F[tgt]['name']) not in EDGE_CUTS: out.append(tgt)
    if use_lit:
        for at,tgt,nm in f['litresolved']:
            if tgt in F and (f['name'],F[tgt]['name']) not in EDGE_CUTS: out.append(tgt)
    return out

sys.setrecursionlimit(100000)
def deepest(root, use_lit=True, extra_cuts=()):
    cuts=set(CUTS)
    for pat in extra_cuts:
        for a,f in F.items():
            if re.search(pat, f['dn']) or re.search(pat, f['name']): cuts.add(a)
    memo={}
    def go(a, stack):
        if a in memo: return memo[a]
        if a in cuts: return (F[a]['frame'], [a], False)
        best=(0,[],False); rec=False
        for t in set(edges(a, use_lit)):
            if t in stack:
                rec=True; continue
            c,p,r = go(t, stack|{t})
            rec = rec or r
            if c>best[0]: best=(c,p,r)
        res=(F[a]['frame']+best[0], [a]+best[1], rec or best[2])
        memo[a]=res
        return res
    memo={}
    return go(root, {root})

def byname(pat):
    r=re.compile(pat)
    return [a for a,f in F.items() if r.search(f['dn']) or r.search(f['name'])]

if __name__=="__main__":
    use_lit = os.environ.get("LIT","1")=="1"
    ec = [x for x in os.environ.get("CUTS","").split("|") if x]
    for pat in sys.argv[1:]:
        hits=byname(pat)
        if not hits: print("NO MATCH",pat); continue
        for a in hits[:4]:
            tot,path,rec = deepest(a, use_lit, ec)
            print(f"\n### {F[a]['dn']} @{a:08x}  DEEPEST SUBTREE = {tot}  recursion_cut={rec}  lit={use_lit}")
            for i,x in enumerate(path):
                print(f"   {'  '*0}{i:2d} {F[x]['frame']:5d}  {F[x]['dn'][:120]}")
