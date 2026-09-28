#!/usr/bin/env python3
"""Deepest-stack walk for POST /files/delete and POST /files/copy.

Frames come from graph.json (the `entry` immediate of every ELF symbol, built by
the earlier stage from the .xt.prop-stripped firmware.elf and cross-validated
1:1 against the -fstack-usage corpus). Strongly connected components are
condensed and charged as the sum of their members' frames, and every
non-trivial SCC crossed is printed, so no recursion is silently summed. The
web_file_browser depth counter is modelled by hand in unroll.py.
"""
import json, sys, collections
from pathlib import Path

BASE = Path("/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/"
            "bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack")
G = json.loads((BASE / "graph.json").read_text())
SEEDS = {k: list(v) for k, v in json.loads((BASE / "rd/seeds.json").read_text()).items()}

# Indirect bridges this firmware makes structurally certain: the only VFS
# registered is LittleFS (components/littlefs_storage), so every esp_vfs_*
# dispatch on a path lands in vfs_littlefs_*.
MY_SEEDS = {
    "esp_vfs_opendir":  ["vfs_littlefs_opendir"],
    "esp_vfs_readdir":  ["vfs_littlefs_readdir"],
    "esp_vfs_rmdir":    ["vfs_littlefs_rmdir"],
    "esp_vfs_mkdir":    ["vfs_littlefs_mkdir"],
    "esp_vfs_unlink":   ["vfs_littlefs_unlink"],
    "esp_vfs_stat":     ["vfs_littlefs_stat"],
    "closedir":         ["vfs_littlefs_closedir"],
    "readdir":          ["vfs_littlefs_readdir"],
    "_stat_r":          ["vfs_littlefs_stat"],
    "vfs_littlefs_readdir": ["vfs_littlefs_readdir_r"],
    "_read_r":          ["vfs_littlefs_read", "vfs_littlefs_pread"],
    "_write_r":         ["vfs_littlefs_write", "vfs_littlefs_pwrite"],
}
for _k, _v in MY_SEEDS.items():
    SEEDS.setdefault(_k, [])
    for _x in _v:
        if _x not in SEEDS[_k]:
            SEEDS[_k].append(_x)

# Only the panic tail is cut: once abort() runs the device is already down.
CUT = {"__assert_func", "abort", "_raise_r", "raise", "_exit", "esp_system_abort",
       "panic_abort", "esp_rom_printf", "esp_rom_vprintf", "_abort"}
EXTRA = set()

DEL = ("_ZN7esphome16web_file_browser14WebFileBrowser17delete_recursive_"
       "ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEj")
COPY = ("_ZN7esphome16web_file_browser14WebFileBrowser15copy_recursive_"
        "ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES9_j")
TTD_W = ("_ZNK7esphome16web_file_browser14WebFileBrowser14tree_too_deep_"
         "ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEj")
TTD_P = TTD_W + "$part$0"
CPF = ("_ZN7esphome16web_file_browser14WebFileBrowser10copy_file_"
       "ERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES9_")
LOG = "_ZN7esphome15esp_log_printf_EiPKciS1_z"
# graph.json drops self-recursion edges; both verified by objdump (call8 to self)
SELF_EDGE = {DEL, COPY}

def dem(s):
    return G.get(s, {}).get("demangled", s)

def frame(s):
    return G.get(s, {}).get("frame", 0)

_succ = {}
def succ(s):
    key = (s, id(EXTRA), len(EXTRA))
    if key in _succ:
        return _succ[key]
    d = G.get(s)
    out = list(d["calls"]) + list(d["tails"]) + list(d.get("litcalls", [])) if d else []
    out += SEEDS.get(s, [])
    if s in SELF_EDGE:
        out.append(s)
    seen, r = set(), []
    for e in out:
        if e in CUT or e in EXTRA or e in seen or e not in G:
            continue
        seen.add(e)
        r.append(e)
    _succ[key] = r
    return r

def reset():
    _succ.clear()

def reach(roots):
    seen, st = set(), list(roots)
    while st:
        s = st.pop()
        if s in seen:
            continue
        seen.add(s)
        st.extend(succ(s))
    return seen

def tarjan(nodes):
    idx, low, on, stk, comps, c = {}, {}, {}, [], [], [0]
    for v in nodes:
        if v in idx:
            continue
        work = [(v, iter(succ(v)))]
        idx[v] = low[v] = c[0]; c[0] += 1; stk.append(v); on[v] = True
        while work:
            u, it = work[-1]; adv = False
            for w in it:
                if w not in nodes:
                    continue
                if w not in idx:
                    idx[w] = low[w] = c[0]; c[0] += 1; stk.append(w); on[w] = True
                    work.append((w, iter(succ(w)))); adv = True; break
                elif on.get(w):
                    low[u] = min(low[u], idx[w])
            if adv:
                continue
            work.pop()
            if work:
                low[work[-1][0]] = min(low[work[-1][0]], low[u])
            if low[u] == idx[u]:
                comp = []
                while True:
                    w = stk.pop(); on[w] = False; comp.append(w)
                    if w == u:
                        break
                comps.append(comp)
    return comps

def deepest(root, show=80, label=None, quiet=False):
    nodes = reach([root])
    comps = tarjan(nodes)
    cid = {s: i for i, c in enumerate(comps) for s in c}
    selfl = {i for i, c in enumerate(comps) if len(c) == 1 and c[0] in succ(c[0])}
    dag = collections.defaultdict(set)
    for s in nodes:
        for t in succ(s):
            if t in cid and cid[t] != cid[s]:
                dag[cid[s]].add(cid[t])
    cost = lambda i: sum(frame(x) for x in comps[i])
    memo = {}
    def best(i):
        if i in memo:
            return memo[i]
        memo[i] = (0, [i])
        b, bp = 0, []
        for j in dag[i]:
            v, p = best(j)
            if v > b:
                b, bp = v, p
        memo[i] = (cost(i) + b, [i] + bp)
        return memo[i]
    tot, path = best(cid[root])
    if not quiet:
        print(f"\n########## {label or dem(root)[:90]}  ->  {tot} B  ({len(nodes)} syms reachable)")
        run = 0
        for i in path[:show]:
            run += cost(i)
            tag = "" if (len(comps[i]) == 1 and i not in selfl) else f"[SCC n={len(comps[i])}] "
            print(f"   {cost(i):5d} {run:6d}  {tag}" + " | ".join(dem(x)[:88] for x in comps[i]))
        nt = [i for i, c in enumerate(comps) if len(c) > 1 or i in selfl]
        if nt:
            print(f"   recursive components in this closure: {len(nt)}")
            for i in nt[:25]:
                print(f"      n={len(comps[i])} charged={cost(i)}: "
                      + " | ".join(dem(x)[:60] for x in comps[i]))
    return tot, path, comps

if __name__ == "__main__":
    for s in sys.argv[1:]:
        deepest(s)
