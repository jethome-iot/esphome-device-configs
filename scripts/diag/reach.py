#!/usr/bin/env python3
"""Mark which frames in the -fstack-usage corpus the ESP-IDF httpd task can
execute, and attribute every frame to the object file it came from.

Inputs (all produced by the two sibling scripts + the build):
  corpus/stack_usage.csv   collect_su.py
  corpus/callgraph.json    callgraph.py
  <build>/<name>.map       the linker map, for object-file attribution

Sets:
  httpd       direct-call closure of the two registered URI handlers, every
              AsyncWebHandler virtual override, and the named indirect bridges
              (newlib/VFS -> LittleFS, the multipart parser callbacks).
  scope-only  symbols in the handler-owning translation units that the direct
              closure does not reach -- either behind an unresolved indirect
              call on the httpd task, or (AsyncEventSource *_main_loop_) on the
              main loop task.
  other-task  esphome/components/api/* -- the API server task, not httpd.
"""

import bisect
import collections
import csv
import json
import re
import sys
from collections import deque
from pathlib import Path

SD = Path(__file__).resolve().parent
MAP = Path(sys.argv[1]) if len(sys.argv) > 1 else None
cg = json.loads((SD / "corpus/callgraph.json").read_text())
su = list(csv.DictReader((SD / "corpus/stack_usage.csv").open()))


def qual_name(pretty: str) -> str:
    """'static esp_err_t ns::C::f(httpd_req_t*)' -> 'ns::C::f'."""
    depth, cut = 0, None
    for i, ch in enumerate(pretty):
        if ch == "<":
            depth += 1
        elif ch == ">":
            depth -= 1
        elif ch == "(" and depth == 0:
            cut = i
            break
    head = pretty[:cut] if cut is not None else pretty
    depth, start = 0, 0
    for i in range(len(head) - 1, -1, -1):
        ch = head[i]
        if ch == ">":
            depth += 1
        elif ch == "<":
            depth -= 1
        elif ch == " " and depth == 0:
            start = i + 1
            break
    return head[start:].strip()


su_by_name = {}
for r in su:
    su_by_name.setdefault(qual_name(r["function"]), []).append(r)

THUNK = re.compile(r"^(non-virtual thunk to |virtual thunk to )")
# GCC clone suffixes and the ABI tag never appear in a .su pretty name.
CLONE = re.compile(r"(\$(?:part|isra|constprop|cold)\$\d+)+$")
ABITAG = re.compile(r"\[abi:[^\]]+\]")
for s, f in cg.items():
    q = qual_name(THUNK.sub("", f["demangled"]))
    f["qual"] = ABITAG.sub("", CLONE.sub("", q))
    f["is_thunk"] = bool(THUNK.match(f["demangled"]))

HTTPD_TU = re.compile(
    r"esphome/components/(web_server|web_server_base|web_server_idf|web_auth"
    r"|web_file_browser|web_device_dashboard|web_automation_editor|web_origin_guard"
    r"|automations|entity_config|config_json|json|md5|sha256|littlefs_storage)/"
)
HTTPD_COMP = re.compile(
    r"^(esp_http_server|http_parser|joltwallet__littlefs|zorxx__multipart-parser)$"
)
OTHER_TU = re.compile(r"esphome/components/api/")

name_scope = {}
for r in su:
    n = qual_name(r["function"])
    if HTTPD_TU.search(r["tu"]) or HTTPD_COMP.match(r["component"]):
        name_scope[n] = "httpd"
    elif OTHER_TU.search(r["tu"]) and n not in name_scope:
        name_scope[n] = "other"

ENTRY_NAMES = {
    "esphome::web_server_idf::AsyncWebServer::request_handler",
    "esphome::web_server_idf::AsyncWebServer::request_post_handler",
}
OVERRIDE = re.compile(
    r"::(canHandle|handleRequest|handleBody|handleUpload|isRequestHandlerTrivial)$"
)
BRIDGE = re.compile(r"^(vfs_littlefs_|esp_littlefs|lfs_|esp_vfs_|multipart_parser_)")


def closure(seeds):
    R, q = set(), deque(seeds)
    while q:
        s = q.popleft()
        if s in R:
            continue
        R.add(s)
        for c in cg[s]["calls"]:
            if c in cg and c not in R:
                q.append(c)
    return R


def hops(seeds):
    d = {s: 0 for s in seeds}
    q = deque(seeds)
    while q:
        s = q.popleft()
        for c in cg[s]["calls"]:
            if c in cg and c not in d:
                d[c] = d[s] + 1
                q.append(c)
    return d


seed_strict = {
    s
    for s, f in cg.items()
    if f["qual"] in ENTRY_NAMES or OVERRIDE.search(f["qual"]) or BRIDGE.match(f["qual"])
}
seed_scope = {s for s, f in cg.items() if name_scope.get(f["qual"]) == "httpd"}
seed_other = {s for s, f in cg.items() if name_scope.get(f["qual"]) == "other"}

R_strict = closure(seed_strict)
R_scope = closure(seed_scope) - R_strict
R_other = closure(seed_other) - R_strict - R_scope
dist = hops(seed_strict)

# ---- object-file attribution from the linker map ---------------------------
obj_starts, obj_names = [], []
if MAP and MAP.exists():
    row = re.compile(
        r"^\s+(0x[0-9a-f]{8,16})\s+(0x[0-9a-f]+)\s+(\S+\.(?:a\([^)]+\)|obj|o))\s*$"
    )
    spans = []
    for line in MAP.open(encoding="utf-8", errors="replace"):
        m = row.match(line)
        if m:
            a, n = int(m.group(1), 16), int(m.group(2), 16)
            if n:
                spans.append((a, a + n, m.group(3)))
    spans.sort()
    obj_starts = [s[0] for s in spans]


def object_of(addr):
    if not obj_starts:
        return ""
    i = bisect.bisect_right(obj_starts, addr) - 1
    if i < 0:
        return ""
    a, b, name = spans[i]
    return name if a <= addr < b else ""


PREBUILT = re.compile(
    r"/lib(c|m|g|gcc|stdc\+\+|supc\+\+|net80211|phy|pp|rtc|coexist|mesh|"
    r"smartconfig|espnow|core|hal|btdm_app|xt_hal)\.a\("
)


def su_gap_reason(r):
    if r["su_bytes"] is not None:
        return ""
    if r["is_thunk"]:
        return "vtable thunk (32 B, no source function)"
    o = r["object"]
    if not o:
        return "no object in map (ROM / linker-provided)"
    if PREBUILT.search("/" + o):
        return "prebuilt archive, compiled without -fstack-usage"
    if "$part$" in r["name"] or "$isra$" in r["name"] or "$constprop$" in r["name"]:
        return "GCC clone of a function that does have a .su row"
    return "name-join miss (template / abi:cxx11); frame read from the prologue"


def emit(R, tag):
    out = []
    for s in R:
        f = cg[s]
        cands = su_by_name.get(f["qual"], [])
        addr = int(f["addr"], 16)
        row = {
            "set": tag,
            "sym": s,
            "name": f["demangled"],
            "addr": f["addr"],
            "frame_dis": f["frame"],
            "entry_imm": f["entry_imm"],
            "extra_sp": f["extra_sp"],
            "dyn_sp": f["dyn_sp"],
            "su_bytes": max((int(c["bytes"]) for c in cands), default=None),
            "qualifier": ",".join(sorted({c["qualifier"] for c in cands}))
            if cands
            else "",
            "su_overloads": len(cands),
            "file": cands[0]["file"] if cands else "",
            "tu": cands[0]["tu"] if cands else "",
            "component": cands[0]["component"] if cands else "",
            "object": object_of(addr),
            "indirect_calls": f["indirect_calls"],
            "hops_from_handler": dist.get(s, ""),
            "is_thunk": f["is_thunk"],
        }
        row["su_gap"] = su_gap_reason(row)
        out.append(row)
    return out


rows = (
    emit(R_strict, "httpd") + emit(R_scope, "scope-only") + emit(R_other, "other-task")
)
rows.sort(key=lambda r: -r["frame_dis"])
(SD / "corpus/reachable.json").write_text(json.dumps(rows, indent=1), encoding="utf-8")
with (SD / "corpus/reachable.csv").open("w", newline="", encoding="utf-8") as fh:
    w = csv.DictWriter(fh, fieldnames=list(rows[0].keys()))
    w.writeheader()
    w.writerows(rows)

print(
    f"strict {len(R_strict)}  scope-only {len(R_scope)}  other-task {len(R_other)}  ELF total {len(cg)}"
)
h = [r for r in rows if r["set"] in ("httpd", "scope-only")]
mism = [
    r
    for r in h
    if r["su_bytes"] is not None
    and r["su_bytes"] != r["frame_dis"]
    and r["su_overloads"] == 1
    and not r["is_thunk"]
]
print(
    f"web-reachable rows {len(h)}; no .su row {sum(1 for r in h if r['su_bytes'] is None)}; "
    f"1:1 join disagreeing with the prologue {len(mism)}"
)

print("\n.su gaps on web-reachable paths:")
for k, v in collections.Counter(r["su_gap"] for r in h if r["su_gap"]).most_common():
    print(f"  {v:>4}  {k}")
print(f"\n{'frame':>6} {'su':>5} d {'hops':>4} {'set':<10} name")
for r in h[:40]:
    print(
        f"{r['frame_dis']:>6} {str(r['su_bytes']):>5} {'D' if r['dyn_sp'] else ' '} "
        f"{str(r['hops_from_handler']):>4} {r['set']:<10} {r['name'][:92]}"
    )

# ---- artifacts for the next stage ------------------------------------------
# 1. the symbols the httpd task enters, and every AsyncWebHandler override
OV = re.compile(r"::(canHandle|handleRequest|handleBody|handleUpload)$")
ep = []
for s, f in cg.items():
    if f["is_thunk"]:
        continue
    kind = (
        "registered httpd_uri_t handler"
        if f["qual"] in ENTRY_NAMES
        else "AsyncWebHandler override"
        if OV.search(f["qual"])
        else None
    )
    if kind:
        ep.append(
            {
                "kind": kind,
                "addr": f["addr"],
                "frame": f["frame"],
                "indirect_calls": f["indirect_calls"],
                "sym": s,
                "name": f["demangled"],
            }
        )
ep.sort(key=lambda r: (r["kind"] != "registered httpd_uri_t handler", r["name"]))
with (SD / "corpus/handler_entrypoints.csv").open(
    "w", newline="", encoding="utf-8"
) as fh:
    w = csv.DictWriter(fh, fieldnames=list(ep[0].keys()))
    w.writeheader()
    w.writerows(ep)

# 2. the esp_http_server frames that are already on the stack when a handler runs
CHAIN = [
    "httpd_thread",
    "httpd_sess_process",
    "httpd_req_new",
    "httpd_uri",
    "httpd_resp_send",
    "httpd_resp_send_chunk",
    "httpd_resp_send_err",
    "httpd_req_recv",
    "httpd_recv_with_opt",
    "httpd_send_all",
    "http_parser_execute",
    "httpd_sess_delete_invalid",
    "esphome::web_server_idf::AsyncWebServer::request_handler",
    "esphome::web_server_idf::AsyncWebServer::request_post_handler",
    "esphome::web_server_idf::AsyncWebServer::request_handler_",
    "esphome::web_server_idf::AsyncWebServer::handle_raw_body_",
    "esphome::web_server_idf::AsyncWebServer::handle_multipart_upload_",
]
byqual = {f["qual"]: (s, f) for s, f in cg.items() if not f["is_thunk"]}
with (SD / "corpus/httpd_task_chain.csv").open("w", newline="", encoding="utf-8") as fh:
    w = csv.writer(fh)
    w.writerow(["name", "addr", "frame", "indirect_calls"])
    for n in CHAIN:
        if n in byqual:
            s, f = byqual[n]
            w.writerow([f["demangled"], f["addr"], f["frame"], f["indirect_calls"]])
        else:
            w.writerow([n, "", "inlined - no distinct symbol", ""])
print("\nwrote corpus/handler_entrypoints.csv and corpus/httpd_task_chain.csv")
