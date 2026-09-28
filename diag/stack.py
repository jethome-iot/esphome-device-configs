#!/usr/bin/env python3
"""Which handler runs deepest on the httpd task's 4352-byte stack.

uxTaskGetStackHighWaterMark is a low-water mark and never recovers, so this is a staircase:
surfaces are exercised one at a time and every new low is attributable to the surface that
just ran. Run it on a freshly booted device.
"""
from __future__ import annotations

import argparse
import sys

import requests
from requests.auth import HTTPDigestAuth

import e2
import e2u  # noqa: F401  registers the unique-URL surfaces


ORDER = [
    "ae_ping",
    "ws_page",
    "ws_rest",
    "ws_detail",
    "ws_command",
    "notfound",
    "auth_none",
    "auth_rejected",
    "origin_rejected",
    "dash_page",
    "dash_info",
    "dash_status",
    "dash_network",
    "dash_auth",
    "dash_capabilities",
    "dash_entities",
    "dash_entity_settings_meta",
    "dash_entity_settings",
    "dash_entity_settings_post",
    "dash_auth_post",
    "fb_info",
    "fb_list",
    "fb_write",
    "fb_read",
    "fb_download",
    "fb_upload",
    "fb_mkdir_delete",
    "fb_rename_copy",
    "ae_list",
    "ae_entities",
    "ae_schema",
    "ae_export",
    "ae_get",
    "ae_save",
    "ws_events",
    "panel_frame",
    "fw_check",
]


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--reps", type=int, default=4)
    ap.add_argument("--only", nargs="*", help="just these surfaces, in this order")
    args = ap.parse_args()

    session = requests.Session()
    session.auth = HTTPDigestAuth("admin", "admin")

    low = e2.last_sample().httpd_hw
    print(f"start: httpd stack headroom {low} B of 4352")
    for name in args.only or ORDER:
        fn = e2.SURFACES[name]
        try:
            for i in range(args.reps):
                fn(session, i)
        except Exception as exc:  # noqa: BLE001
            print(f"{name:28s} request failed: {exc}", file=sys.stderr)
        after = e2.wait_samples(1, name)
        mark = ""
        if after.httpd_hw < low:
            mark = f"  <-- new low, -{low - after.httpd_hw} B"
            low = after.httpd_hw
        print(f"{name:28s} headroom {after.httpd_hw:5d} B   used {4352 - after.httpd_hw:5d} B{mark}")
    print(f"\nworst httpd stack headroom: {low} B of 4352 ({100 * (4352 - low) / 4352:.0f}% used)")


if __name__ == "__main__":
    main()
