#!/usr/bin/env python3
"""Deepest stack path with the web_file_browser depth counter modelled explicitly.

MAX_RECURSION_DEPTH = 8 (web_file_browser.cpp:29) and every guard is
`if (depth > MAX) return`, so a frame is entered for depths 0..9 and the depth-9
frame only logs and returns. copy_recursive_'s rollback calls
delete_recursive_(dst, depth) at the SAME depth, so the two share one budget.
tree_too_deep_ was split by GCC: the wrapper (32 B) holds the depth test and
$part$0 (144 B) does the work, so one level past the first costs 176 B.
"""
import sys, paths as P

G = P.G
BYNAME = {}
for a, v in G.items():
    BYNAME.setdefault(v['name'], []).append(a)

def sym(substr, want_part=None):
    hits = [a for a, v in G.items() if substr in v['name'] and
            (want_part is None or (('$part$0' in v['name']) == want_part))]
    assert len(hits) == 1, (substr, [G[h]['name'] for h in hits])
    return hits[0]

DEL   = sym('WebFileBrowser17delete_recursive_')
COPY  = sym('WebFileBrowser15copy_recursive_')
TTD_W = sym('WebFileBrowser14tree_too_deep_', want_part=False)
TTD_P = sym('WebFileBrowser14tree_too_deep_', want_part=True)
DEPTH_NODES = {DEL, COPY, TTD_W, TTD_P}
MAXD = 8

def deepest_d(addr, depth):
    """(cost, chain) where `depth` is the web_file_browser recursion depth at addr."""
    memo = {}
    def rec(a, d, stack):
        key = (a, d)
        if key in memo:
            return memo[key]
        if a in stack and a not in DEPTH_NODES:
            return (0, [])            # non-depth-counted cycle: cut
        v = G[a]
        best = (0, [])
        for c in P.callees(a):
            if c not in G:
                continue
            if c in DEPTH_NODES:
                # copy/delete/tree_too_deep recursion: depth+1, except copy's
                # rollback into delete_recursive_ which keeps the same depth
                nd = d if (a == COPY and c == DEL) else d + 1
                if nd > MAXD:
                    continue
                r = rec(c, nd, stack | {a})
            else:
                r = rec(c, d, stack | {a})
            if r[0] > best[0]:
                best = r
        res = (v['frame'] + best[0], [(a, d)] + best[1])
        memo[key] = res
        return res
    sys.setrecursionlimit(60000)
    return rec(addr, depth, frozenset())

def chain(label, entry_frames, addr, depth=0, excl=None, limit=60):
    P.EXCLUDE = excl or set()
    cost, path = deepest_d(addr, depth)
    total = sum(f for _, f in entry_frames) + cost
    print(f"\n=== {label}")
    print(f"    prefix {sum(f for _,f in entry_frames)} + subtree {cost} = {total} B")
    for n, f in entry_frames:
        print(f"    {f:5d}  {n}")
    for a, d in path[:limit]:
        tag = f" [depth {d}]" if a in DEPTH_NODES else ""
        print(f"    {G[a]['frame']:5d}  {G[a]['name'][:92]}{tag}")
    if len(path) > limit:
        print(f"    ... {len(path)-limit} more")
    return total

if __name__ == '__main__':
    HTTPD = [('httpd_thread', 128), ('httpd_sess_enum', 32), ('httpd_process_session', 32),
             ('httpd_sess_process', 32), ('httpd_req_new', 160), ('httpd_uri', 48)]
    POST = HTTPD + [('AsyncWebServer::request_post_handler', 160),
                    ('AsyncWebServer::request_handler_', 48),
                    ('AuthMiddlewareHandler::handleRequest', 32),
                    ('WebOriginGuard::handleRequest', 32),
                    ('WebFileBrowser::handleRequest', 80)]
    HD = sym('WebFileBrowser22handle_delete_request_')
    HC = sym('WebFileBrowser20handle_copy_request_')
    for excl, tag in [(set(), 'static worst case'), ({'__assert_func', 'abort'}, 'no assert/abort tail')]:
        print('\n' + '#' * 78)
        print('#', tag)
        chain(f'POST /files/delete  [{tag}]', POST, HD, depth=-1, excl=excl)
        chain(f'POST /files/copy    [{tag}]', POST, HC, depth=-1, excl=excl)
