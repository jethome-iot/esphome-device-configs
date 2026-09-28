#!/usr/bin/env python3
"""Deepest-stack-path search over the recursive-descent call graph.

cost(n) = frame(n) + max(cost(callee)).  Cycles are entered at most once per
path (so a reported number is a lower bound for a recursion unless the caller
unrolls it explicitly); every cycle crossed is reported.
"""
import json, sys, collections  # noqa

G = json.load(open('/tmp/claude-1000/-home-alexey-dev-esphome-device-configs/bf4965c5-a19f-4694-a820-14d815214413/scratchpad/httpd-stack/slice-delete-copy/rdgraph.json'))

NAME2ADDR = collections.defaultdict(list)
for a, v in G.items():
    NAME2ADDR[v['name']].append(a)

# Indirect dispatch bridges that are structurally certain for this firmware:
# the only filesystem VFS registered is LittleFS (components/littlefs_storage),
# so every esp_vfs / newlib reent dispatch on a file path lands in vfs_littlefs_*.
BRIDGES = {
    'esp_vfs_opendir': ['vfs_littlefs_opendir'],
    'esp_vfs_readdir': ['vfs_littlefs_readdir'],
    'esp_vfs_rmdir':   ['vfs_littlefs_rmdir'],
    'esp_vfs_mkdir':   ['vfs_littlefs_mkdir'],
    'closedir':        ['vfs_littlefs_closedir'],
    '_stat_r':         ['vfs_littlefs_stat'],
    '_unlink_r':       ['vfs_littlefs_unlink'],
    '_open_r':         ['vfs_littlefs_open'],
    '_read_r':         ['vfs_littlefs_read', 'vfs_littlefs_pread'],
    '_write_r':        ['vfs_littlefs_write', 'vfs_littlefs_pwrite'],
    '_close_r':        ['vfs_littlefs_close'],
    'esp_vfs_utime':   ['vfs_littlefs_utime'],
    'vfs_littlefs_readdir': ['vfs_littlefs_readdir_r'],
    # littlefs block device behind lfs_bd_*
    'lfs_bd_read':  ['littlefs_esp_part_read'],
    'lfs_bd_prog':  ['littlefs_esp_part_write', 'littlefs_esp_part_sync'],
    'lfs_bd_erase': ['littlefs_esp_part_erase'],
    'lfs_bd_flush': ['littlefs_esp_part_write', 'littlefs_esp_part_sync'],
    # littlefs match / traverse callbacks
    'lfs_dir_fetchmatch': ['lfs_dir_find_match', 'lfs_fs_parent_match'],
    'lfs_fs_traverse_': ['lfs_alloc_lookahead'],
    'lfs_dir_traverse$constprop$0': ['lfs_dir_traverse_filter', 'lfs_dir_commit_commit',
                                     'lfs_dir_commit_size'],
}

EXCLUDE = set()          # names never followed (e.g. abort paths) - filled by caller


def callees(a):
    v = G[a]
    out = []
    for n in v['calls'] + BRIDGES.get(v['name'], []):
        if n in EXCLUDE:
            continue
        for t in NAME2ADDR.get(n, []):
            out.append(t)
    return out


def deepest(root_addr, extra_edges=None):
    """iterative DFS with memo; cycles are cut (entered once)."""
    memo = {}
    cycles = set()
    onstack = []
    onset = set()

    def rec(a):
        if a in memo:
            return memo[a]
        if a in onset:
            cycles.add((onstack[-1], a))
            return (0, [])
        onstack.append(a)
        onset.add(a)
        best = (0, [])
        ce = list(callees(a))
        if extra_edges:
            ce += [t for t in extra_edges.get(a, [])]
        for c in ce:
            if c not in G:
                continue
            r = rec(c)
            if r[0] > best[0]:
                best = r
        onstack.pop()
        onset.discard(a)
        res = (G[a]['frame'] + best[0], [a] + best[1])
        # only memoise when no cycle passed through here
        memo[a] = res
        return res

    sys.setrecursionlimit(100000)
    return rec(root_addr), cycles


def show(label, addr, limit=40):
    (cost, path), cycles = deepest(addr)
    print(f"\n### {label}  ({addr})  total {cost} B")
    for a in path[:limit]:
        v = G[a]
        print(f"   {v['frame']:5d}  {a}  {v['name'][:110]}")
    if len(path) > limit:
        print(f"   ... {len(path)-limit} more")
    if cycles:
        print("   cycles cut on this walk:")
        for f, t in sorted(cycles):
            print(f"     {G[f]['name'][:60]} -> {G[t]['name'][:60]}")
    return cost, path, cycles


if __name__ == '__main__':
    for pat in sys.argv[1:]:
        for a, v in sorted(G.items()):
            if pat in v['name']:
                show(v['name'][:70], a)
