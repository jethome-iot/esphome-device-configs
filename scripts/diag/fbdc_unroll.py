#!/usr/bin/env python3
"""POST /files/delete and POST /files/copy: worst-case httpd-task stack.

The web_file_browser depth counter is modelled by hand. MAX_RECURSION_DEPTH = 8
(components/web_file_browser/web_file_browser.cpp:29) and every guard is
`if (depth > MAX_RECURSION_DEPTH) { log; return; }`, i.e. the check is INSIDE the
callee, so a frame is pushed for depths 0..9 and the depth-9 frame still runs an
ESP_LOGE before returning.
"""
import sys
import fbdc_walk as W

F = W.frame

# --- leaf subtree costs -------------------------------------------------------
UPPER_CUT = set()
# Flash encryption is off in this build (CONFIG_SECURE_FLASH_ENC_ENABLED unset),
# so esp_partition_read/write take the plain esp_flash_* branch and never the
# mmap/encrypted one. lock_init_generic is a one-time lazy newlib lock init.
# _dtoa_r is only reached for %e/%f/%g and no format string on these paths has
# one. __assert_func ends in abort(), which is already a dead device.
REAL_CUT = {"esp_partition_mmap", "spi_flash_mmap", "spi_flash_munmap", "esp_mmu_map",
            "esp_flash_write_encrypted", "esp_flash_read_encrypted",
            "lock_init_generic", "_dtoa_r", "__assert_func"}

def leaf(sym, cut):
    W.EXTRA = set(cut)
    W.reset()
    tot, path, comps = W.deepest(sym, quiet=True)
    return tot, [x for i in path for x in comps[i]]

# --- the httpd prefix ---------------------------------------------------------
HTTPD = ["httpd_thread", "httpd_sess_enum", "httpd_process_session",
         "httpd_sess_process", "httpd_req_new", "httpd_uri"]
POST_RAW = ["_ZN7esphome14web_server_idf14AsyncWebServer20request_post_handlerEP9httpd_req",
            "_ZN7esphome14web_server_idf14AsyncWebServer16handle_raw_body_EP9httpd_reqPKc",
            "_ZN7esphome15web_server_base8internal21AuthMiddlewareHandler13handleRequestE"
            "PNS_14web_server_idf21AsyncWebServerRequestE",
            "_ZN7esphome16web_origin_guard14WebOriginGuard13handleRequestE"
            "PNS_14web_server_idf21AsyncWebServerRequestE",
            "_ZN7esphome16web_file_browser14WebFileBrowser13handleRequestE"
            "PNS_14web_server_idf21AsyncWebServerRequestE"]
POST_FORM = [POST_RAW[0],
             "_ZNK7esphome14web_server_idf14AsyncWebServer16request_handler_EPNS0_21AsyncWebServerRequestE"] + POST_RAW[2:]
HD = "_ZN7esphome16web_file_browser14WebFileBrowser22handle_delete_request_EPNS_14web_server_idf21AsyncWebServerRequestE"
HC = "_ZN7esphome16web_file_browser14WebFileBrowser20handle_copy_request_EPNS_14web_server_idf21AsyncWebServerRequestE"

def show(names, tag):
    tot = 0
    print(f"  -- {tag}")
    for n in names:
        tot += F(n)
        print(f"     {F(n):5d} {tot:6d}  {W.dem(n)[:88]}")
    return tot

def main():
    for cutname, cut in (("UPPER BOUND (only the abort tail cut)", UPPER_CUT),
                         ("REALISTIC (see REAL_CUT)", REAL_CUT)):
        print("\n" + "#" * 78)
        print("#", cutname)
        print("#" * 78)

        leaves = {}
        for name, sym in [("ESP_LOGE", W.LOG), ("opendir", "esp_vfs_opendir"),
                          ("readdir", "esp_vfs_readdir"), ("closedir", "closedir"),
                          ("stat", "stat"), ("remove/unlink", "remove"),
                          ("rmdir", "esp_vfs_rmdir"), ("mkdir", "esp_vfs_mkdir"),
                          ("strerror", "strerror"), ("std::string cat", "_ZStplIcSt11char_traitsIcESaIcEENSt7__cxx1112basic_stringIT_T0_T1_EERKS8_PKS5_"),
                          ("copy_file_", W.CPF)]:
            if sym not in W.G:
                print("  MISSING", sym); continue
            c, ch = leaf(sym, cut)
            leaves[name] = (c, ch)
        print("\n  leaf subtree costs (frame of the leaf included):")
        for k, (c, ch) in sorted(leaves.items(), key=lambda kv: -kv[1][0]):
            print(f"     {c:5d}  {k}")
        for k in ("ESP_LOGE", "remove/unlink", "rmdir", "mkdir", "copy_file_"):
            print(f"\n  chain for {k}:")
            run = 0
            for s in leaves[k][1]:
                run += F(s)
                print(f"     {F(s):5d} {run:6d}  {W.dem(s)[:88]}")

        # ---- leaves each recursion level can reach -------------------------
        # delete_recursive_ / tree_too_deep_ / copy_recursive_ callee sets come
        # straight from the disassembly (verified by objdump).
        del_leaves = ["ESP_LOGE", "opendir", "readdir", "closedir", "stat",
                      "remove/unlink", "rmdir", "strerror", "std::string cat"]
        ttd_leaves = ["opendir", "readdir", "closedir", "stat", "std::string cat"]
        cp_leaves = del_leaves + ["mkdir", "copy_file_"]
        dl = max((leaves[k][0], k) for k in del_leaves)
        tl = max((leaves[k][0], k) for k in ttd_leaves)
        cl = max((leaves[k][0], k) for k in cp_leaves)
        print(f"\n  deepest leaf reachable from delete_recursive_ : {dl[0]} ({dl[1]})")
        print(f"  deepest leaf reachable from tree_too_deep_     : {tl[0]} ({tl[1]})")
        print(f"  deepest leaf reachable from copy_recursive_    : {cl[0]} ({cl[1]})")

        fdel, fcp = F(W.DEL), F(W.COPY)
        fw, fp = F(W.TTD_W), F(W.TTD_P)
        print(f"\n  frames: delete_recursive_={fdel} copy_recursive_={fcp} "
              f"tree_too_deep_ wrapper={fw} $part$0={fp} "
              f"handle_delete_request_={F(HD)} handle_copy_request_={F(HC)}")

        for pname, pre in (("POST form-urlencoded", POST_FORM),
                           ("POST other Content-Type (raw body)", POST_RAW)):
            print("\n" + "=" * 70)
            print(f"  prefix: {pname}")
            base = show(HTTPD, "esp_http_server task") + show(pre, "esphome dispatch")
            print(f"     ===== prefix {base}")

            # ---------------- POST /files/delete ----------------
            # tree_too_deep_ runs first and REFUSES any tree that would take
            # delete_recursive_ to depth 9, so delete_recursive_ only ever runs
            # depths 0..8 on this route. The two are sequential, so the route
            # costs max(one, the other).
            ttd = fp + 8 * (fw + fp)            # $part$0 d=0..8 + wrapper d=1..8
            ttd_deep = ttd + tl[0]
            ttd_full = ttd + fw                 # wrapper d=9 instead of a leaf
            dr = 9 * fdel                       # depths 0..8
            dr_deep = dr + dl[0]
            dtot = base + F(HD) + max(ttd_deep, ttd_full, dr_deep)
            print(f"\n  POST /files/delete")
            print(f"     handle_delete_request_          {F(HD)}")
            print(f"     tree_too_deep_ d=0..8           {ttd}   (+deepest leaf {tl[0]} = {ttd_deep};"
                  f" or + wrapper d=9 = {ttd_full})")
            print(f"     delete_recursive_ d=0..8        {dr}   (+deepest leaf {dl[0]} = {dr_deep})")
            print(f"     TOTAL                           {dtot}")

            # ---------------- POST /files/copy ----------------
            # No pre-check, so copy_recursive_ itself is entered at depth 9 and
            # the depth-9 frame runs an ESP_LOGE. Rollback calls
            # delete_recursive_(dst, depth) at the SAME depth, so from a copy
            # frame at depth 8 the rollback still reaches delete depth 9.
            a = 10 * fcp + leaves["ESP_LOGE"][0]                  # copy d=0..9, log at d=9
            b = 9 * fcp + 2 * fdel + leaves["ESP_LOGE"][0]        # copy d=0..8, delete d=8,9
            c = 9 * fcp + fdel + dl[0]                            # copy d=0..8, delete d=8 + leaf
            d = 9 * fcp + cl[0]                                   # copy d=0..8 + deepest leaf
            best = max((a, "copy d=0..9 + ESP_LOGE at d=9"),
                       (b, "copy d=0..8 + rollback delete d=8,9 + ESP_LOGE at d=9"),
                       (c, "copy d=0..8 + rollback delete d=8 + deepest leaf"),
                       (d, "copy d=0..8 + deepest leaf"))
            ctot = base + F(HC) + best[0]
            print(f"\n  POST /files/copy")
            print(f"     handle_copy_request_            {F(HC)}")
            print(f"     (a) {a}  copy d=0..9 + ESP_LOGE at d=9")
            print(f"     (b) {b}  copy d=0..8 + rollback delete d=8,9 + ESP_LOGE at d=9")
            print(f"     (c) {c}  copy d=0..8 + rollback delete d=8 + deepest leaf")
            print(f"     (d) {d}  copy d=0..8 + deepest leaf")
            print(f"     TOTAL                           {ctot}   [{best[1]}]")
            print(f"\n  >>> delete {dtot}  copy {ctot}   (head-room floor 844, whole stack 4352)")

if __name__ == "__main__":
    main()
