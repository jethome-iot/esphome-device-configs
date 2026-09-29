#!/usr/bin/env python3
"""Show that a stalled /events client used to cost the device a session slot, and no longer does.

Written to be run twice — against the firmware without the fix and against the one with it — and to
say which of the two it is looking at rather than only pass or fail. Issue #70; the fix is the
backport of upstream ESPHome PR #17800 in components/web_server_idf.

What breaks without the fix: a client that stops reading had its session object freed while
esp_http_server still held the pointer, and the socket was never closed. Each occurrence lost about
8 KB of internal heap and one of the device's seven session slots for good, and the write into the
released block corrupted whatever took it over, so the device panicked later somewhere unrelated.

So there are two observable symptoms, and this checks both after every cycle:

  slots   a probe /events must still open. Without the fix the slots run out and it stops opening.
  reboot  /api/device/status carries uptime_s and reset_reason. Uptime going backwards means the
          device restarted, and reset_reason says whether it panicked.

    # against the broken firmware — expected to find the bug
    scripts/diag/events_stall.py --base http://10.0.0.5 --auth admin:admin --expect broken

    # against the fixed one — expected to find it gone
    scripts/diag/events_stall.py --base http://10.0.0.5 --auth admin:admin --expect fixed

Either way the exit status is 0 when the observation matches --expect, so the pair of runs is the
proof. Without --expect it just reports what it saw. --out writes the run as JSON and --compare
prints two runs side by side.

A run that cannot stall a client at all — wrong credentials, device already out of slots before it
started, network in the way — reports INCONCLUSIVE and never BROKEN. Only an observation made after
a client really was stalled is evidence of anything.

Standard library only. It opens and closes /events and reads /api/device/status; it writes nothing
to the device and never touches an OTA route.
"""

import argparse
import hashlib
import http.client
import json
import socket
import sys
import time
import urllib.error
import urllib.request
from urllib.parse import urlparse

STATUS_PATH = "/api/device/status"


# --------------------------------------------------------------------------------------- transport


def _digest_fields(header: str) -> dict[str, str]:
    """The key="value" pairs of a WWW-Authenticate: Digest header."""
    fields: dict[str, str] = {}
    for part in header.split(","):
        key, _, value = part.partition("=")
        if value:
            fields[key.strip().lower().removeprefix("digest ")] = value.strip().strip(
                '"'
            )
    return fields


def _digest_response(user: str, password: str, uri: str, fields: dict[str, str]) -> str:
    """RFC 7616 MD5 qop=auth. ESPHome's digest is stateless with a fresh nonce per challenge and no
    replay protection, so one challenge can answer several requests."""
    realm, nonce = fields.get("realm", ""), fields.get("nonce", "")
    cnonce = hashlib.md5(f"{nonce}{uri}".encode()).hexdigest()[:16]
    ha1 = hashlib.md5(f"{user}:{realm}:{password}".encode()).hexdigest()
    ha2 = hashlib.md5(f"GET:{uri}".encode()).hexdigest()
    digest = hashlib.md5(
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
        f'response="{digest}"',
    ]
    if fields.get("opaque"):
        parts.append(f'opaque="{fields["opaque"]}"')
    return "Digest " + ", ".join(parts)


class Device:
    """JSON reads go through urllib, which answers digest challenges itself. A stalled /events
    client needs a small receive window and must not read its body, so it gets a connection whose
    socket is built by hand — SO_RCVBUF has to be set before connect() to shrink the window the
    device is told about."""

    def __init__(self, base: str, auth: str | None, timeout: float):
        url = urlparse(base)
        self.host, self.port = url.hostname, url.port or 80
        self.base = f"http://{self.host}:{self.port}"
        self.timeout = timeout
        self.user, _, self.password = (auth or "").partition(":")
        self.challenge: dict[str, str] = {}
        # An empty ProxyHandler suppresses the default one, which would otherwise send every
        # request to whatever HTTP_PROXY happens to be set in the environment.
        handlers: list = [urllib.request.ProxyHandler({})]
        if auth:
            mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
            mgr.add_password(None, self.base, self.user, self.password)
            handlers += [
                urllib.request.HTTPDigestAuthHandler(mgr),
                urllib.request.HTTPBasicAuthHandler(mgr),
            ]
        self.opener = urllib.request.build_opener(*handlers)

    # -- JSON reads

    def _json(self, path: str) -> dict:
        try:
            with self.opener.open(self.base + path, timeout=self.timeout) as response:
                return json.loads(response.read())
        except (urllib.error.URLError, OSError, ValueError):
            return {}

    def status(self) -> dict:
        return self._json(STATUS_PATH)

    def sensor(self, path: str) -> float | None:
        payload = self._json(path)
        for key in ("value", "state"):
            if key in payload:
                try:
                    return float(payload[key])
                except (TypeError, ValueError):
                    pass
        return None

    # -- /events

    def _connection(self, rcvbuf: int | None) -> http.client.HTTPConnection | None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        if rcvbuf:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
        sock.settimeout(self.timeout)
        try:
            sock.connect((self.host, self.port))
        except OSError:
            sock.close()
            return None
        conn = http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        conn.sock = sock  # already connected, so connect() is never called
        return conn

    def _get_head(self, conn: http.client.HTTPConnection, path: str, auth: str | None):
        """Sends the request and returns the response with its body unread."""
        conn.putrequest("GET", path, skip_accept_encoding=True)
        conn.putheader("Accept", "text/event-stream")
        if auth:
            conn.putheader("Authorization", auth)
        conn.endheaders()
        return conn.getresponse()

    def refresh_challenge(self, path: str) -> bool:
        """Takes a digest challenge from an unauthenticated request, on its own connection."""
        conn = self._connection(None)
        if conn is None:
            return False
        try:
            response = self._get_head(conn, path, None)
            header = response.getheader("WWW-Authenticate", "")
            response.read()
            if header:
                self.challenge = _digest_fields(header)
            return bool(self.challenge)
        except (http.client.HTTPException, OSError):
            return False
        finally:
            conn.close()

    def open_events(
        self, path: str, rcvbuf: int | None
    ) -> tuple[http.client.HTTPConnection | None, int]:
        """Opens /events without reading the body. The caller owns and closes the connection."""
        for _ in range(2):
            auth = (
                _digest_response(self.user, self.password, path, self.challenge)
                if (self.user and self.challenge)
                else None
            )
            conn = self._connection(rcvbuf)
            if conn is None:
                return None, 0
            try:
                response = self._get_head(conn, path, auth)
            except (http.client.HTTPException, OSError):
                conn.close()
                return None, 0
            if response.status != 401 or not self.user:
                return conn, response.status
            # Stale or missing challenge: take a fresh one and try once more.
            response.read()
            conn.close()
            if not self.refresh_challenge(path):
                return None, 401
        return None, 401


# ------------------------------------------------------------------------------------------ the run


def run(device: Device, args) -> dict:
    first = device.status()
    if not first:
        print(f"Cannot read {STATUS_PATH} — check --base and --auth.", file=sys.stderr)
        return {}
    if device.user and not device.refresh_challenge(args.path):
        print(
            f"Cannot get a digest challenge for {args.path} — check --auth.",
            file=sys.stderr,
        )
        return {}

    baseline_heap = device.sensor(args.sample) if args.sample else None
    record = {
        "cycles_requested": args.cycles,
        "hold_s": args.hold,
        "uptime_at_start_s": first.get("uptime_s"),
        "reset_reason_at_start": first.get("reset_reason"),
        "sample_path": args.sample,
        "baseline_heap": baseline_heap,
        "leak_threshold": args.leak_threshold,
        "cycles": [],
    }

    print(
        f"uptime at start {first.get('uptime_s')}s, reset_reason {first.get('reset_reason')!r}"
    )
    if baseline_heap is not None:
        print(f"{args.sample} at start: {baseline_heap:.0f}")
    print(f"\nStalling {args.cycles} /events clients for {args.hold:.0f}s each\n")

    previous_uptime = first.get("uptime_s") or 0
    for cycle in range(1, args.cycles + 1):
        stalled, status = device.open_events(args.path, args.rcvbuf)
        if stalled is None or status != 200:
            record["cycles"].append(
                {"cycle": cycle, "stall_opened": False, "stall_status": status}
            )
            print(
                f"cycle {cycle:2}: could not open a client to stall (status {status or 'no answer'})"
            )
            break
        # Hold it without reading: the receive window closes and the device eventually gives up.
        time.sleep(args.hold)
        stalled.close()
        time.sleep(1.5)

        probe, probe_status = device.open_events(args.path, None)
        if probe is not None:
            probe.close()
        state = device.status()
        heap = device.sensor(args.sample) if args.sample else None
        uptime = state.get("uptime_s")
        rebooted = uptime is not None and uptime < previous_uptime
        if uptime is not None:
            previous_uptime = uptime

        record["cycles"].append(
            {
                "cycle": cycle,
                "stall_opened": True,
                "probe_status": probe_status,
                "uptime_s": uptime,
                "reset_reason": state.get("reset_reason"),
                "rebooted": rebooted,
                "heap": heap,
            }
        )
        line = f"cycle {cycle:2}: probe /events -> {str(probe_status) or 'no answer':<9} uptime {uptime}"
        if heap is not None:
            line += f"  heap {heap:.0f}"
        if rebooted:
            line += f"  *** REBOOT, reset_reason={state.get('reset_reason')!r} ***"
        print(line)
        if rebooted and args.stop_on_reboot:
            print(
                "stopping: the device restarted, which is one of the failures this looks for"
            )
            break

    record["final_heap"] = device.sensor(args.sample) if args.sample else None
    return verdict(record)


def verdict(record: dict) -> dict:
    cycles = record.get("cycles", [])
    completed = [c for c in cycles if c.get("stall_opened")]
    reboots = [c for c in cycles if c.get("rebooted")]
    # Only a probe taken after a client really was stalled says anything about session slots.
    refused = [c for c in completed if c.get("probe_status") != 200]

    record["completed_cycles"] = len(completed)
    record["reboots"] = len(reboots)
    record["slots_lost_at_cycle"] = refused[0]["cycle"] if refused else None
    record["stalling_failed_at_cycle"] = next(
        (c["cycle"] for c in cycles if not c.get("stall_opened")), None
    )

    # The heap is the earliest and most direct symptom: the unfixed firmware never reclaims about
    # 8 KB per stalled client, so a couple of cycles show it long before the seven slots run out.
    leaking = False
    if record.get("baseline_heap") is not None and record.get("final_heap") is not None:
        record["heap_delta"] = record["final_heap"] - record["baseline_heap"]
        if (
            completed and not reboots
        ):  # a restart resets the heap, so the delta means nothing then
            record["heap_per_cycle"] = record["heap_delta"] / len(completed)
            leaking = -record["heap_per_cycle"] >= record["leak_threshold"]

    if not completed:
        # Nothing was ever stalled, so nothing was tested. Never call this broken.
        record["state"] = "inconclusive"
    elif reboots or record["slots_lost_at_cycle"] is not None or leaking:
        record["state"] = "broken"
    elif len(completed) == record["cycles_requested"]:
        record["state"] = "fixed"
    else:
        record["state"] = "inconclusive"
    return record


def report(record: dict) -> None:
    print("\n--- what this run saw ---")
    print(
        f"clients actually stalled:  {record['completed_cycles']}/{record['cycles_requested']}"
    )
    if record["slots_lost_at_cycle"] is not None:
        print(
            f"a probe /events stopped being served at cycle {record['slots_lost_at_cycle']}"
        )
    elif record["completed_cycles"]:
        print("a probe /events was served after every stalled client")
    if record["stalling_failed_at_cycle"] is not None:
        print(
            f"could not open a client to stall at cycle {record['stalling_failed_at_cycle']}"
        )
    print(f"device restarts during the run: {record['reboots']}")
    if "heap_delta" in record:
        print(
            f"{record['sample_path']}: {record['baseline_heap']:.0f} -> "
            f"{record['final_heap']:.0f} ({record['heap_delta']:+.0f})"
        )
    if "heap_per_cycle" in record:
        print(
            f"per stalled client: {record['heap_per_cycle']:+.0f} B"
            f"  (the leak this looks for is about -8000; --leak-threshold is {record['leak_threshold']:.0f})"
        )

    state = record["state"]
    if state == "broken":
        print(
            "\nBROKEN: stalled clients cost the device session slots, or it restarted while they did."
        )
    elif state == "fixed":
        print(
            "\nFIXED: every stalled client gave its session slot back and the device never restarted."
        )
    else:
        print(
            "\nINCONCLUSIVE: not enough clients were stalled to conclude anything. Check the"
            "\ncredentials and that the device had slots free before the run, then try again."
        )


def compare(before_path: str, after_path: str) -> int:
    with open(before_path) as fh:
        before = json.load(fh)
    with open(after_path) as fh:
        after = json.load(fh)
    rows = [
        ("state", before["state"], after["state"]),
        (
            "clients stalled",
            f"{before['completed_cycles']}/{before['cycles_requested']}",
            f"{after['completed_cycles']}/{after['cycles_requested']}",
        ),
        (
            "slots lost at cycle",
            before["slots_lost_at_cycle"] or "-",
            after["slots_lost_at_cycle"] or "-",
        ),
        ("device restarts", before["reboots"], after["reboots"]),
        ("heap delta", before.get("heap_delta", "-"), after.get("heap_delta", "-")),
        (
            "heap per client",
            f"{before.get('heap_per_cycle', 0):+.0f}"
            if "heap_per_cycle" in before
            else "-",
            f"{after.get('heap_per_cycle', 0):+.0f}"
            if "heap_per_cycle" in after
            else "-",
        ),
    ]
    width = max(len(r[0]) for r in rows)
    print(f"{'':<{width}}  {'before':>20}  {'after':>20}")
    for name, b, a in rows:
        print(f"{name:<{width}}  {str(b):>20}  {str(a):>20}")
    if before["state"] == "broken" and after["state"] == "fixed":
        print("\nThe fix does what it claims: broken before, fixed after.")
        return 0
    print(
        "\nThe pair does not demonstrate the fix. Read the two runs before believing either."
    )
    return 1


def main() -> int:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--base", help="e.g. http://10.0.0.5")
    p.add_argument("--auth", help="user:password; the factory default is admin:admin")
    p.add_argument("--path", default="/events")
    p.add_argument(
        "--cycles",
        type=int,
        default=10,
        help="stalled clients to open and drop; there are 7 slots",
    )
    p.add_argument(
        "--hold",
        type=float,
        default=40.0,
        help="seconds to hold each stalled client. The device gives up after 20s without send progress.",
    )
    p.add_argument(
        "--rcvbuf", type=int, default=2048, help="SO_RCVBUF; the kernel may round it up"
    )
    p.add_argument(
        "--sample",
        help="a numeric REST getter for the heap column, e.g. /sensor/Heap%%20Free",
    )
    p.add_argument(
        "--expect",
        choices=["broken", "fixed"],
        help="exit non-zero unless the run sees this",
    )
    p.add_argument(
        "--stop-on-reboot",
        action="store_true",
        help="stop at the first restart instead of continuing",
    )
    p.add_argument("--out", help="write the run to this file as JSON")
    p.add_argument(
        "--compare",
        nargs=2,
        metavar=("BEFORE", "AFTER"),
        help="compare two --out files and exit",
    )
    p.add_argument(
        "--leak-threshold",
        type=float,
        default=2000.0,
        help="bytes lost per stalled client above which the run is called broken; the unfixed "
        "firmware loses about 8000 and a fixed one should sit near zero",
    )
    p.add_argument("--timeout", type=float, default=10.0)
    args = p.parse_args()

    if args.compare:
        return compare(*args.compare)
    if not args.base:
        p.error("--base is required unless --compare is given")

    record = run(Device(args.base, args.auth, args.timeout), args)
    if not record:
        return 2
    report(record)
    if args.out:
        with open(args.out, "w") as fh:
            json.dump(record, fh, indent=2)
        print(f"\nwritten to {args.out}")

    if args.expect and record["state"] != args.expect:
        print(f"\nThis run saw {record['state']}, and --expect said {args.expect}.")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
