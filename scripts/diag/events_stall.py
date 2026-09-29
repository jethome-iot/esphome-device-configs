#!/usr/bin/env python3
"""Prove that a stalled /events client no longer costs the device a session slot.

The bug this checks for (issue #70, upstream ESPHome PR #17800): a client that stops reading
had its session object freed while esp_http_server still held the pointer, and the socket was
never closed. Each occurrence leaked ~8 KB and held one of the device's seven session slots for
good, so the eighth stalled client could no longer be served at all.

That is the assertion here, and it needs no instrumentation on the device: stall and drop a
client N times, then open one more /events and see whether it still answers. On the unfixed
firmware this fails once the slots are gone; on the fixed one every cycle returns its slot.

  scripts/diag/events_stall.py --base http://10.0.0.5 --auth admin:admin --cycles 10

With --sample it also reads a numeric REST getter before and after, so a build carrying the
stock `debug:` component reports the heap that each cycle did or did not give back:

  scripts/diag/events_stall.py --base http://10.0.0.5 --auth admin:admin \
      --sample /sensor/Heap%20Free

Standard library only. Read-only against the device: it opens and closes /events and nothing
else, so it is safe to run against a unit in service.
"""

import argparse
import hashlib
import socket
import sys
import time
from urllib.parse import urlparse

READ_HEAD = (
    256  # Enough to take the status line and the first event, then stop reading.
)


def parse_challenge(header: str) -> dict[str, str]:
    """The comma-separated key="value" pairs of a WWW-Authenticate: Digest header."""
    fields: dict[str, str] = {}
    for part in header.split(",", -1):
        if "=" not in part:
            continue
        key, _, value = part.partition("=")
        fields[key.strip().lower().removeprefix("digest ")] = value.strip().strip('"')
    return fields


def digest_header(
    user: str, password: str, method: str, uri: str, challenge: dict[str, str]
) -> str:
    """An RFC 7616 MD5 qop=auth response. The device issues a fresh nonce per challenge."""
    realm = challenge.get("realm", "")
    nonce = challenge.get("nonce", "")
    opaque = challenge.get("opaque")
    cnonce = hashlib.md5(f"{nonce}{uri}".encode()).hexdigest()[:16]
    ha1 = hashlib.md5(f"{user}:{realm}:{password}".encode()).hexdigest()
    ha2 = hashlib.md5(f"{method}:{uri}".encode()).hexdigest()
    response = hashlib.md5(
        f"{ha1}:{nonce}:00000001:{cnonce}:auth:{ha2}".encode()
    ).hexdigest()
    parts = [
        f'username="{user}"',
        f'realm="{realm}"',
        f'nonce="{nonce}"',
        f'uri="{uri}"',
        "qop=auth",
        "nc=00000001",
        f'cnonce="{cnonce}"',
        f'response="{response}"',
    ]
    if opaque:
        parts.append(f'opaque="{opaque}"')
    return "Digest " + ", ".join(parts)


class Device:
    def __init__(self, base: str, auth: str | None, timeout: float):
        url = urlparse(base)
        self.host = url.hostname
        self.port = url.port or 80
        self.timeout = timeout
        self.user, _, self.password = (auth or "").partition(":")

    def _connect(self, rcvbuf: int | None) -> socket.socket:
        sock = socket.create_connection((self.host, self.port), timeout=self.timeout)
        if rcvbuf:
            # A small receive buffer is what lets the window close while the device still has
            # the connect burst to push: config, one event per sorting group, then every entity.
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        return sock

    def _request(self, sock: socket.socket, path: str, extra: str = "") -> bytes:
        request = f"GET {path} HTTP/1.1\r\nHost: {self.host}\r\nAccept: text/event-stream\r\n{extra}Connection: keep-alive\r\n\r\n"
        sock.sendall(request.encode())
        return sock.recv(READ_HEAD)

    def open_events(self, path: str, rcvbuf: int | None) -> tuple[socket.socket, int]:
        """Opens /events, answering a digest challenge if one comes. Reads only the head."""
        sock = self._connect(rcvbuf)
        head = self._request(sock, path)
        status = self._status(head)
        if status != 401 or not self.user:
            return sock, status

        header = ""
        for line in head.decode("latin-1").split("\r\n"):
            if line.lower().startswith("www-authenticate:"):
                header = line.partition(":")[2].strip()
        sock.close()
        if not header:
            return sock, status

        auth = digest_header(
            self.user, self.password, "GET", path, parse_challenge(header)
        )
        sock = self._connect(rcvbuf)
        head = self._request(sock, path, extra=f"Authorization: {auth}\r\n")
        return sock, self._status(head)

    @staticmethod
    def _status(head: bytes) -> int:
        try:
            return int(head.split(b" ", 2)[1])
        except (IndexError, ValueError):
            return 0

    def read_number(self, path: str) -> float | None:
        """A numeric REST getter, for the optional heap column."""
        import json

        sock, status = self.open_events(path, None)
        try:
            if status != 200:
                return None
            body = b""
            while b"\r\n\r\n" not in body:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                body += chunk
            payload = body.partition(b"\r\n\r\n")[2]
            while True:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                payload += chunk
            data = json.loads(payload[payload.find(b"{") : payload.rfind(b"}") + 1])
            for key in ("value", "state"):
                if key in data:
                    return float(data[key])
        except (OSError, ValueError):
            return None
        finally:
            sock.close()
        return None


def main() -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--base", required=True, help="e.g. http://10.0.0.5")
    p.add_argument("--auth", help="user:password; the factory default is admin:admin")
    p.add_argument("--path", default="/events")
    p.add_argument(
        "--cycles", type=int, default=10, help="stalled clients to open and drop"
    )
    p.add_argument(
        "--hold",
        type=float,
        default=40.0,
        help="seconds to hold each stalled client. The device gives up after 20 s without send "
        "progress, so leave margin above that.",
    )
    p.add_argument(
        "--rcvbuf", type=int, default=2048, help="SO_RCVBUF; the kernel may round it up"
    )
    p.add_argument(
        "--sample",
        help="a numeric REST getter to read before and after, e.g. /sensor/Heap%%20Free",
    )
    p.add_argument("--timeout", type=float, default=10.0)
    args = p.parse_args()

    device = Device(args.base, args.auth, args.timeout)

    before = device.read_number(args.sample) if args.sample else None
    if args.sample and before is None:
        print(
            f"note: could not read {args.sample}; continuing without the heap column",
            file=sys.stderr,
        )

    print(
        f"Stalling {args.cycles} /events clients for {args.hold:.0f}s each, then asking for one more\n"
    )
    stalled = 0
    for cycle in range(1, args.cycles + 1):
        sock, status = device.open_events(args.path, args.rcvbuf)
        if status != 200:
            print(
                f"cycle {cycle:2}: /events answered {status} — the device is already out of slots or refusing"
            )
            sock.close()
            break
        stalled += 1
        # Hold it open without reading: the send window closes and the device eventually gives up.
        time.sleep(args.hold)
        sock.close()
        print(f"cycle {cycle:2}: stalled and dropped")

    # The assertion. On the unfixed firmware the slots are gone and this cannot be served.
    time.sleep(2.0)
    sock, status = device.open_events(args.path, None)
    sock.close()
    after = device.read_number(args.sample) if args.sample else None

    print()
    print(f"stalled clients completed: {stalled}/{args.cycles}")
    if before is not None and after is not None:
        print(
            f"{args.sample}: {before:.0f} -> {after:.0f}  (delta {after - before:+.0f})"
        )
    print(f"a fresh /events after all of them: {status}")

    if stalled < args.cycles:
        print(
            "\nFAIL: the device stopped accepting /events partway — session slots were not returned."
        )
        return 1
    if status != 200:
        print(
            "\nFAIL: a fresh /events is refused after the run — session slots were not returned."
        )
        return 1
    print("\nPASS: every stalled client returned its session slot.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
