#!/usr/bin/env python3
"""Deepest-stack-path over the corpus callgraph, with cut-sets and bounded recursion unrolling."""
import json,sys
SP='/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack'
CG=json.load(open(SP+'/corpus/callgraph.json'))
BY_ADDR={v['addr'].lower():k for k,v in CG.items()}

# Paths that only run after the device has already decided to die, or are pure
# diagnostics inside a lock-timeout assert. Reported separately, cut from the main number.
ABORT_CUT={'__assert_func','abort','_esp_error_check_failed','esp_system_abort','panic_abort',
           '_ZSt9terminatev','_ZN10__cxxabiv111__terminateEPFvvE','__cxa_throw',
           'esp_log_write','esp_log_writev','_esp_system_abort'}
# The loop task owns everything past this; a frame there is charged to the loop's own 8192.
LOOP_CUT={'_ZN7esphome8loop_job14LoopDispatcher11run_on_loopEPNS_9ComponentEOSt8functionIFbvEE',
          '_ZN7esphome9Component11set_timeoutEjOSt8functionIFvvEE',
          '_ZN7esphome9Component5deferEPKcOSt8functionIFvvEE'}

def res(x):
    x=str(x).lower().replace('0x','')
    if x in BY_ADDR: return BY_ADDR[x]
    if x in CG: return x
    c=[k for k,v in CG.items() if x in v['demangled'] or x in k]
    if len(c)==1: return c[0]
    raise SystemExit("ambiguous/none for %r:\n"%x+"\n".join("  %s %5d %s"%(CG[k]['addr'],CG[k]['frame'],CG[k]['demangled'][:130]) for k in c[:30]))

def deepest(root, cut=(), unroll=None, extra=None):
    """unroll: {sym: n} -> that sym may appear n times nested. Others cut at first repeat."""
    cut=set(cut); unroll=unroll or {}; extra=extra or {}
    memo={}
    def go(sym, counts):
        if sym in cut or sym not in CG: return (0,[])
        n=counts.get(sym,0)
        lim=unroll.get(sym,1)
        if n>=lim: return (0,[])
        key=(sym,tuple(sorted((k,v) for k,v in counts.items() if k in unroll)))
        if key in memo: return memo[key]
        c2=dict(counts); c2[sym]=n+1
        f=CG[sym]['frame']
        best=(0,[])
        for c in list(CG[sym]['calls'])+extra.get(sym,[]):
            b,p=go(c,c2)
            if b>best[0]: best=(b,p)
        r=(f+best[0], [(sym,f)]+best[1])
        memo[key]=r
        return r
    return go(root,{})

def pr(tag,root,**kw):
    b,p=deepest(res(root),**kw)
    print('### %s  = %d B' % (tag,b))
    for s,f in p: print('   %5d  %s' % (f, CG[s]['demangled'][:135]))
    print()
    return b,p

if __name__=='__main__':
    a=sys.argv[1:]
    cut=ABORT_CUT|LOOP_CUT
    pr(a[0], a[0], cut=cut)
