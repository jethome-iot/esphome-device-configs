#!/usr/bin/env python3
"""Variants that make every request DIFFERENT, so a container keyed by something the
request carries would grow. The main harness sends identical requests, which a keyed cache
would absorb after the first one."""
from __future__ import annotations

import sys
import uuid

import e2


def notfound_unique(s, i):
    e2._get(s, f"/api/device/no-such-{i}-{uuid.uuid4().hex[:8]}")


def fb_read_unique(s, i):
    e2._get(s, f"/files/read?path=/missing-{i}.txt")


def fb_write_unique(s, i):
    """A new file per request: the LittleFS directory and config_json's record list both
    grow with distinct names, and the files are removed again at the end."""
    s.post(
        e2.BASE + f"/files/write?path=/u{i}.txt",
        data=b"z" * 64,
        headers={"Content-Type": "text/plain"},
        timeout=30,
    )
    s.post(e2.BASE + f"/files/delete", data={"path": f"/u{i}.txt"}, timeout=30)


def es_unique_type(s, i):
    e2._get(s, f"/api/device/entity-settings?type=nope{i}")


def es_unique_source(s, i):
    e2._get(s, f"/api/device/entity-settings?type=switch&source_name=ghost{i}")


def ae_get_unique(s, i):
    e2._get(s, f"/automation-editor/api/get?id={100 + i}")


def ws_entity_unique(s, i):
    e2._get(s, f"/sensor/NoSuchSensor{i}")


def auth_unique_user(s, i):
    import requests
    from requests.auth import HTTPDigestAuth

    requests.get(
        e2.BASE + "/api/device/info", auth=HTTPDigestAuth(f"user{i}", "nope"), timeout=30
    )


e2.SURFACES.update(
    {
        "notfound_unique": notfound_unique,
        "fb_read_unique": fb_read_unique,
        "fb_write_unique": fb_write_unique,
        "es_unique_type": es_unique_type,
        "es_unique_source": es_unique_source,
        "ae_get_unique": ae_get_unique,
        "ws_entity_unique": ws_entity_unique,
        "auth_unique_user": auth_unique_user,
    }
)

if __name__ == "__main__":
    sys.exit(e2.main())
