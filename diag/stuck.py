#!/usr/bin/env python3
"""Drive web_server v3's "stuck EventSource" path and watch what the device does next.

The path under test is web_server_idf.cpp:899-907: after MAX_CONSECUTIVE_SEND_FAILURES
(2500, ~20 s) the component sets fd_ = 0, which makes AsyncEventSource::loop() delete the
session object — while the httpd socket is left open still holding that pointer in
sock_db.ctx with free_ctx = AsyncEventSourceResponse::destroy. The question this script
answers: what gets logged when that socket is finally torn down?
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import os
import re
import socket
import sys
import time

LOG = os.environ.get(
    "E2_LOG",
    "/home/alexey/dev/wt-esphome-device-configs/heap-qemu/devices/JXD/"
    ".esphome/build/jxd-r6-e1eth-lcd-qemu/qemu.log",
)
HOST, PORT = "127.0.0.1", int(os.environ.get("E2_PORT", "8091"))
USER, PASSWORD = "admin", "admin"
ANSI = re.compile(rb"\x1b\[[0-9;]*m")


def log_size() -> int:
    return os.path.getsize(LOG)


def log_since(pos: int) -> str:
    with open(LOG, "rb") as fh:
        fh.seek(pos)
        return ANSI.sub(b"", fh.read()).decode("utf-8", "replace")


def _h(*parts: str) -> str:
    return hashlib.md5(":".join(parts).encode()).hexdigest()


def digest_header(challenge: str, method: str, uri: str) -> str:
    fields = dict(re.findall(r'(\w+)="?([^",]+)"?', challenge))
    realm, nonce = fields["realm"], fields["nonce"]
    qop = fields.get("qop", "auth").split(",")[0].strip()
    cnonce = base64.b16encode(os.urandom(4)).decode()
    nc = "00000001"
    ha1, ha2 = _h(USER, realm, PASSWORD), _h(method, uri)
    resp = _h(ha1, nonce, nc, cnonce, qop, ha2)
    return (
        f'Digest username="{USER}", realm="{realm}", nonce="{nonce}", uri="{uri}", '
        f'qop={qop}, nc={nc}, cnonce="{cnonce}", response="{resp}"'
    )


def open_events(rcvbuf: int = 2048) -> socket.socket:
    """An authenticated GET /events on a raw socket whose receive buffer is tiny, so the
    device's send window closes after the first few events."""
    uri = "/events"
    probe = socket.create_connection((HOST, PORT), timeout=10)
    probe.sendall(f"GET {uri} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\n\r\n".encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = probe.recv(2048)
        if not chunk:
            break
        head += chunk
    probe.close()
    m = re.search(rb"WWW-Authenticate:\s*(.+)\r\n", head, re.I)
    if not m:
        sys.exit(f"no digest challenge on {uri}: {head[:200]!r}")
    auth = digest_header(m.group(1).decode(), "GET", uri)

    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    sock.connect((HOST, PORT))
    sock.sendall(
        f"GET {uri} HTTP/1.1\r\nHost: {HOST}:{PORT}\r\nAuthorization: {auth}\r\n\r\n".encode()
    )
    sock.settimeout(5)
    # Read only the response head, then stop reading entirely.
    try:
        sock.recv(256)
    except socket.timeout:
        pass
    return sock


def flood(seconds: float) -> None:
    """State changes, so the unread SSE client has something it cannot be sent. QEMU's slirp
    networking buffers generously, so a client that merely stops reading is not enough on its
    own: the bytes have to exceed what slirp will hold."""
    import requests
    from requests.auth import HTTPDigestAuth

    s = requests.Session()
    s.auth = HTTPDigestAuth(USER, PASSWORD)
    deadline = time.time() + seconds
    sent = 0
    while time.time() < deadline:
        for n in (1, 2, 3, 4, 5, 6):
            try:
                s.post(f"http://{HOST}:{PORT}/switch/Relay%20{n}/toggle", data="", timeout=10)
                sent += 1
            except Exception:  # noqa: BLE001
                pass
            if time.time() >= deadline:
                break
    s.close()
    print(f"    flooded {sent} state changes")


def churn(n: int = 40) -> None:
    """Allocation traffic, so a freed session block is handed out again before the socket
    teardown writes into it."""
    import requests
    from requests.auth import HTTPDigestAuth

    s = requests.Session()
    s.auth = HTTPDigestAuth(USER, PASSWORD)
    for _ in range(n):
        try:
            s.get(f"http://{HOST}:{PORT}/api/device/entities", timeout=10)
            s.get(f"http://{HOST}:{PORT}/api/device/entity-settings-meta", timeout=10)
        except Exception:  # noqa: BLE001
            pass
    s.close()


INTERESTING = re.compile(
    r"(Closing stuck EventSource|Event source connection closed|CORRUPT HEAP|"
    r"abort\(\)|Guru Meditation|assert failed|heap_caps|LoadProhibited|StoreProhibited|"
    r"rst:0x|HEAPCSV)"
)


def report(pos: int, label: str) -> None:
    lines = [ln for ln in log_since(pos).splitlines() if INTERESTING.search(ln)]
    print(f"--- {label} ---")
    for ln in lines:
        print("   ", ln.strip())


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=1, help="stuck connections to create")
    ap.add_argument("--hold", type=float, default=32.0, help="seconds to hold unread")
    ap.add_argument("--churn", type=int, default=40, help="requests between delete and close")
    ap.add_argument("--keep-open", action="store_true", help="never close the client sockets")
    ap.add_argument("--flood", action="store_true", help="drive state changes during the hold")
    args = ap.parse_args()

    socks = []
    for i in range(args.n):
        pos = log_size()
        print(f"[{i}] opening /events with a 2 KB receive buffer and not reading it")
        sock = open_events()
        if args.flood:
            flood(args.hold)
        else:
            time.sleep(args.hold)
        report(pos, f"{i}: after holding {args.hold}s unread")

        pos = log_size()
        if args.churn:
            print(f"[{i}] {args.churn * 2} requests of allocation churn")
            churn(args.churn)
        if args.keep_open:
            socks.append(sock)
            print(f"[{i}] leaving the socket open")
        else:
            print(f"[{i}] closing the client socket")
            sock.close()
        time.sleep(6)
        report(pos, f"{i}: after churn + close")

    for sock in socks:
        sock.close()
    if socks:
        pos = log_size()
        time.sleep(8)
        report(pos, "after closing every held socket")


if __name__ == "__main__":
    main()
