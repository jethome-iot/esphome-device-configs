#!/usr/bin/env python3
"""What every HTTP request costs on the 4352-byte esp_http_server task stack, before and
around the route-specific work: the IDF prefix, ESPHome's dispatch, the canHandle scan,
web_origin_guard's check, web_auth's digest verification, and the response/log tails.

Run with no arguments for the scenario table, `seeds` to print the synthetic edges, or a
symbol name (plus symbols to cut) for one ad-hoc walk.

Base graph: graph.json (resynced disassembly, scripts/diag/resync.py).
Every synthetic edge added here is an indirect call the disassembly cannot
resolve; each one is listed in SEEDS with the reason.
"""
import json, os, re, sys
from pathlib import Path

# graph.json is scripts/diag/resync.py's output: frame sizes and call edges read off a
# resynchronised objdump of firmware.elf. Point GRAPH_JSON at it.
GRAPH = Path(os.environ.get("GRAPH_JSON", "graph.json"))
G = json.load(open(GRAPH))
DEM = {}
for k, v in G.items():
    DEM.setdefault(v.get('demangled') or k, k)

def S(name):
    if name in G: return name
    if name in DEM: return DEM[name]
    rx = re.compile(re.escape(name))
    hits = [k for k, v in G.items() if rx.search(v.get('demangled') or k)]
    if len(hits) == 1: return hits[0]
    raise SystemExit(f"{name!r}: {len(hits)} matches" + ("\n  " + "\n  ".join((G[h].get('demangled') or h) for h in hits[:12]) if hits else ""))

def frame(n):
    v = G.get(n)
    return 0 if v is None else (v['entry'] or 0) + v.get('extra_sp', 0)

# ---- the handler objects actually in AsyncWebServer::handlers_, in setup order ----
CANH = {
  'CrossOriginRefuser': 'esphome::web_origin_guard::CrossOriginRefuser::canHandle',
  'WebDeviceDashboard': 'esphome::web_device_dashboard::WebDeviceDashboard::canHandle',
  'CaptivePortal':      'esphome::captive_portal::CaptivePortal::canHandle',
  'AsyncEventSource':   'esphome::web_server_idf::AsyncEventSource::canHandle(esphome::web_server_idf::AsyncWebServerRequest*) const',
  'WebServer':          'non-virtual thunk to esphome::web_server::WebServer::canHandle',
  'WebFileBrowser':     'esphome::web_file_browser::WebFileBrowser::canHandle',
  'WebAutomationEditor':'esphome::web_automation_editor::WebAutomationEditor::canHandle',
  'OTARequestHandler':  'esphome::web_server::OTARequestHandler::canHandle(esphome::web_server_idf::AsyncWebServerRequest*) const',
}
HREQ = {
  'CrossOriginRefuser': 'esphome::web_origin_guard::CrossOriginRefuser::handleRequest',
  'WebDeviceDashboard': 'esphome::web_device_dashboard::WebDeviceDashboard::handleRequest',
  'CaptivePortal':      'esphome::captive_portal::CaptivePortal::handleRequest',
  'AsyncEventSource':   'esphome::web_server_idf::AsyncEventSource::handleRequest',
  'WebServer':          'non-virtual thunk to esphome::web_server::WebServer::handleRequest',
  'WebFileBrowser':     'esphome::web_file_browser::WebFileBrowser::handleRequest',
  'WebAutomationEditor':'esphome::web_automation_editor::WebAutomationEditor::handleRequest',
  'OTARequestHandler':  'esphome::web_server::OTARequestHandler::handleRequest',
}
HBODY = {
  'CrossOriginRefuser': 'esphome::web_origin_guard::CrossOriginRefuser::handleBody',
  'WebDeviceDashboard': 'esphome::web_device_dashboard::WebDeviceDashboard::handleBody',
  'WebFileBrowser':     'esphome::web_file_browser::WebFileBrowser::handleBody',
  'WebAutomationEditor':'esphome::web_automation_editor::WebAutomationEditor::handleBody',
}
HUP = {
  'CrossOriginRefuser': 'esphome::web_origin_guard::CrossOriginRefuser::handleUpload',
  'WebFileBrowser':     'esphome::web_file_browser::WebFileBrowser::handleUpload',
  'OTARequestHandler':  'esphome::web_server::OTARequestHandler::handleUpload',
}
# handlers wrapped by AuthMiddlewareHandler (add_handler) and, for our own three, by WebOriginGuard
AUTH_WRAPPED = ['WebDeviceDashboard','AsyncEventSource','WebServer','WebFileBrowser','WebAutomationEditor','OTARequestHandler']
GUARD_WRAPPED = ['WebDeviceDashboard','WebFileBrowser','WebAutomationEditor']

MW_CAN   = 'esphome::web_server_base::internal::MiddlewareHandler::canHandle'
AUTH_REQ = 'esphome::web_server_base::internal::AuthMiddlewareHandler::handleRequest'
AUTH_BODY= 'esphome::web_server_base::internal::AuthMiddlewareHandler::handleBody'
AUTH_UP  = 'esphome::web_server_base::internal::AuthMiddlewareHandler::handleUpload'
G_CAN    = 'esphome::web_origin_guard::WebOriginGuard::canHandle'
G_REQ    = 'esphome::web_origin_guard::WebOriginGuard::handleRequest'
G_BODY   = 'esphome::web_origin_guard::WebOriginGuard::handleBody'
G_UP     = 'esphome::web_origin_guard::WebOriginGuard::handleUpload'

SEEDS = []
def seed(src, dsts, why):
    s = S(src)
    for d in dsts:
        SEEDS.append((s, S(d), why))

seed('httpd_sess_enum', ['httpd_process_session'],
     'httpd_sess_enum takes a callback; httpd_thread passes httpd_process_session (the only caller)')
seed('httpd_uri', ['esphome::web_server_idf::AsyncWebServer::request_handler(httpd_req*)',
                   'esphome::web_server_idf::AsyncWebServer::request_post_handler'],
     'uri->handler(req): the only three registered httpd_uri_t entries')
seed('esphome::web_server_idf::AsyncWebServer::request_handler_',
     [MW_CAN, AUTH_REQ] + [CANH['CrossOriginRefuser'], HREQ['CrossOriginRefuser'], CANH['CaptivePortal'], HREQ['CaptivePortal']],
     'vtable slots 8/12 on each handlers_ entry: the two unwrapped handlers plus the Middleware wrapper')
seed(MW_CAN, [CANH[h] for h in AUTH_WRAPPED] + [G_CAN], 'MiddlewareHandler::canHandle -> next_->canHandle')
seed(AUTH_REQ, [HREQ[h] for h in AUTH_WRAPPED] + [G_REQ], 'MiddlewareHandler::handleRequest inlined into AuthMiddlewareHandler::handleRequest -> next_->handleRequest')
seed(AUTH_BODY, [HBODY[h] for h in HBODY] + [G_BODY], 'AuthMiddlewareHandler::handleBody -> next_->handleBody')
seed(AUTH_UP, [HUP[h] for h in HUP] + [G_UP], 'AuthMiddlewareHandler::handleUpload -> next_->handleUpload')
seed(G_CAN, [CANH[h] for h in GUARD_WRAPPED], 'WebOriginGuard::canHandle -> next_->canHandle')
seed(G_REQ, [HREQ[h] for h in GUARD_WRAPPED], 'WebOriginGuard::handleRequest -> next_->handleRequest')
seed(G_BODY, [HBODY[h] for h in GUARD_WRAPPED], 'WebOriginGuard::handleBody -> next_->handleBody')
seed(G_UP, [HUP['WebFileBrowser']], 'WebOriginGuard::handleUpload -> next_->handleUpload')
seed('esphome::web_server_idf::AsyncWebServer::handle_raw_body_(httpd_req*, char const*)',
     [MW_CAN, AUTH_REQ, AUTH_BODY, CANH['CrossOriginRefuser'], HREQ['CrossOriginRefuser'], HBODY['CrossOriginRefuser'],
      CANH['CaptivePortal'], HREQ['CaptivePortal']],
     'handle_raw_body_ scans canHandle then calls handleBody/handleRequest through the vtable')
seed('esphome::web_server_idf::AsyncWebServer::handle_multipart_upload_(httpd_req*, char const*)',
     [MW_CAN, AUTH_REQ, AUTH_UP, CANH['CrossOriginRefuser'], HREQ['CrossOriginRefuser'], HUP['CrossOriginRefuser']],
     'handle_multipart_upload_ scans canHandle then calls handleUpload/handleRequest through the vtable')

EXTRA = {}
for s, d, _ in SEEDS:
    EXTRA.setdefault(s, set()).add(d)

def succ(n, cut):
    v = G.get(n)
    out = set()
    if v: out |= set(v['calls']) | set(v['litcalls']) | set(v['tails'])
    out |= EXTRA.get(n, set())
    return [c for c in out if c in G and c not in cut]

def deepest(start, cut=frozenset()):
    # iterative DFS longest path with SCC condensation
    nodes, st = set(), [start]
    while st:
        n = st.pop()
        if n in nodes: continue
        nodes.add(n); st.extend(succ(n, cut))
    # Tarjan
    index, low, onstk, stack, comp, comps = {}, {}, set(), [], {}, []
    cnt = [0]
    for root in nodes:
        if root in index: continue
        work = [(root, iter(succ(root, cut)))]
        index[root] = low[root] = cnt[0]; cnt[0]+=1; stack.append(root); onstk.add(root)
        while work:
            n, it = work[-1]; adv = False
            for c in it:
                if c not in nodes: continue
                if c not in index:
                    index[c] = low[c] = cnt[0]; cnt[0]+=1; stack.append(c); onstk.add(c)
                    work.append((c, iter(succ(c, cut)))); adv = True; break
                elif c in onstk: low[n] = min(low[n], index[c])
            if adv: continue
            work.pop()
            if work: low[work[-1][0]] = min(low[work[-1][0]], low[n])
            if low[n] == index[n]:
                ms = []
                while True:
                    m = stack.pop(); onstk.discard(m); ms.append(m)
                    if m == n: break
                ci = len(comps); comps.append(ms)
                for m in ms: comp[m] = ci
    cedges = {i:set() for i in range(len(comps))}
    for n in nodes:
        for c in succ(n, cut):
            if c in nodes and comp[c] != comp[n]: cedges[comp[n]].add(comp[c])
    ccost = [sum(frame(m) for m in ms) for ms in comps]
    rec = [len(ms) > 1 or (len(ms)==1 and ms[0] in succ(ms[0], cut)) for ms in comps]
    order, state = [], {}
    for r in range(len(comps)):
        if r in state: continue
        stk = [(r, False)]
        while stk:
            n, done = stk.pop()
            if done: order.append(n); state[n]=2; continue
            if state.get(n): continue
            state[n]=1; stk.append((n,True))
            for d in cedges[n]:
                if not state.get(d): stk.append((d,False))
    best, path = {}, {}
    for ci in order:
        b, bp = 0, []
        for d in cedges[ci]:
            v = best.get(d, 0)
            if v > b: b, bp = v, [d]+path.get(d, [])
        best[ci] = ccost[ci]+b; path[ci] = bp
    ci = comp[start]
    chain = [ci]+path.get(ci, [])
    return best[ci], [(comps[c], ccost[c], rec[c]) for c in chain]

def show(n): return (G[n].get('demangled') or n) if n in G else n

def run(label, start, cut=(), base=0):
    cs = frozenset(S(c) for c in cut)
    t, ch = deepest(S(start), cs)
    print(f"\n### {label}")
    print(f"depth from {show(S(start))} = {t}   (+base {base} = {t+base})")
    for ms, c, r in ch:
        tag = '>' if r else ' '
        if len(ms) == 1: print(f"   {tag} {c:5d}  {show(ms[0])}")
        else: print(f"   {tag} {c:5d}  SCC[{len(ms)}] " + ", ".join(f"{show(m)}({frame(m)})" for m in ms))
    return t



# --------------------------------------------------------------------------------------------
# Scenarios for the per-request fixed cost. Frames that are only reached after the program has
# already decided to die (__assert_func, abort) and the IDF ESP_LOGE branches inside the
# allocator are cut, because they answer a different question than "what does a served request
# cost". Each scenario names what it cuts.
# --------------------------------------------------------------------------------------------
FATAL = ['__assert_func', 'abort']
ALLOC_ERR = ['esp_log_timestamp', '_realloc_r', 'esp_log']   # ESP_LOGE inside heap_caps/esp_cache
NOREENTRY = ['httpd_sess_enum']                              # the dispatch loop is not recursion
NOLOG = ['esphome::esp_log_printf_']
NOFLOAT = ['_dtoa_r']

# httpd_thread 128 + httpd_sess_enum 32 + httpd_process_session 32 + httpd_sess_process 32
# + httpd_req_new 160 + httpd_uri 48; httpd_server and httpd_parse_req are inlined.
P_IDF = 432
P_GET = P_IDF + 80 + 48        # request_handler + request_handler_
P_POST = P_IDF + 160           # request_post_handler

SCENARIOS = [
    ('every request: canHandle scan (worst of the eight handlers_ entries)',
     'esphome::web_server_base::internal::MiddlewareHandler::canHandle', NOFLOAT, P_GET),
    ('web_origin_guard: cross-origin refusal',
     'esphome::web_origin_guard::CrossOriginRefuser::handleRequest', NOFLOAT, P_GET),
    ('web_auth: digest verification, no log branch',
     'esphome::web_server_idf::AsyncWebServerRequest::authenticate', NOLOG, P_GET + 32 + 32),
    ('web_auth: digest verification, "Only Digest" ESP_LOGW branch',
     'esphome::web_server_idf::AsyncWebServerRequest::authenticate', NOFLOAT, P_GET + 32 + 32),
    ('web_auth: 401 challenge',
     'esphome::web_server_idf::AsyncWebServerRequest::requestAuthentication', NOFLOAT, P_GET + 32 + 32),
    ('one ESPHome ESP_LOGx on the httpd task, non-float format',
     'esphome::esp_log_printf_', NOFLOAT, 0),
    ('one ESPHome ESP_LOGx on the httpd task, %f/%g format',
     'esphome::esp_log_printf_', [], 0),
    ('every response: httpd_resp_send', 'httpd_resp_send', NOFLOAT, 0),
    ('every chunked response: httpd_resp_send_chunk', 'httpd_resp_send_chunk', NOFLOAT, 0),
    ('every error response: httpd_resp_send_err', 'httpd_resp_send_err', NOFLOAT, 0),
    ('one std::string / operator new', 'malloc', [], 0),
    ('GET: deepest reachable route, no ESPHome log',
     'esphome::web_server_idf::AsyncWebServer::request_handler(httpd_req*)', NOLOG, P_IDF),
    ('GET: deepest reachable route, ESPHome log allowed',
     'esphome::web_server_idf::AsyncWebServer::request_handler(httpd_req*)', [], P_IDF),
    ('POST: deepest reachable route, no ESPHome log',
     'esphome::web_server_idf::AsyncWebServer::request_post_handler', NOLOG, P_IDF),
    ('POST: deepest reachable route, ESPHome log allowed',
     'esphome::web_server_idf::AsyncWebServer::request_post_handler', [], P_IDF),
]

def all_scenarios():
    for label, start, extra, base in SCENARIOS:
        run(label, start, FATAL + ALLOC_ERR + NOREENTRY + list(extra), base=base)


if __name__ == '__main__':
    if sys.argv[1:] and sys.argv[1] == 'seeds':
        for s, d, why in SEEDS: print(f"{show(s)}\n   -> {show(d)}\n   ({why})")
    elif sys.argv[1:]:
        start = sys.argv[1]; cut = sys.argv[2:]
        run(start, start, cut)
    else:
        all_scenarios()
