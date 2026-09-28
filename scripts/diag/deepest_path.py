import json, os, sys
from pathlib import Path

# The graph written by callgraph_lit.py; override with GRAPH=<path>.
G = json.loads(Path(os.environ.get("GRAPH", sys.argv[1] if len(sys.argv) > 1 else "graph.json")).read_text())
BYADDR = {v['addr']: k for k, v in G.items()}
PANIC = {k for k, v in G.items() if v['demangled'] in (
    '__assert_func','abort','_exit','panic_abort','esp_system_abort','_esp_error_check_failed',
    'esp_restart','std::terminate()','__cxa_throw','__cxa_pure_virtual','_Unwind_Resume',
    '__gnu_cxx::__verbose_terminate_handler()','esp_task_wdt_isr_user_handler','__cxa_begin_catch')}

def find(sub, exact=False):
    return [k for k, v in G.items() if (v['demangled'] == sub if exact else sub in v['demangled'])]
def K(addr): return BYADDR[addr]

def edges(sym, tails=True):
    n = G[sym]
    e = [(c,'call') for c in n['calls'] if c in G] + [(c,'lit') for c in n['lit'] if c in G]
    if tails: e += [(c,'tail') for c in n['tails'] if c in G]
    return e

def deepest(seed, exclude=(), tails=True, tail_cost=True):
    exclude = set(exclude) | PANIC
    memo, onstack, cyc = {}, set(), set()
    def cost(s, kind): return 0 if (kind=='tail' and not tail_cost) else G[s]['frame']
    def go(s):
        if s in memo: return memo[s]
        if s in onstack:
            cyc.add(s); return (0, [])
        onstack.add(s); best = (0, [])
        for c, kind in edges(s, tails):
            if c in exclude: continue
            sub, p = go(c)
            t = cost(c, kind) + sub
            if t > best[0]: best = (t, [(c, cost(c,kind), kind)] + p)
        onstack.discard(s); memo[s] = best
        return best
    sub, p = go(seed)
    return G[seed]['frame'] + sub, [(seed, G[seed]['frame'], 'seed')] + p, cyc

def pr(label, seed, **kw):
    tot, path, cyc = deepest(seed, **kw)
    print(f'\n### {label}  TOTAL {tot}')
    acc = 0
    for s, f, kind in path:
        acc += f
        dy = ' DYN' if G[s]['dyn_sp'] else ''
        print(f'  {acc:6d} += {f:5d} [{kind:4s}]{dy} {G[s]["addr"]} {G[s]["demangled"][:120]}')
    if cyc:
        print('  cycles cut:', sorted(G[c]['demangled'][:90] for c in cyc))
    return tot, path
