#!/usr/bin/env python3
"""Show that a stalled /events client used to cost the device a session slot, and no longer does.

Written to be run twice — against the firmware without the fix and against the one with it — and to
say which of the two it is looking at rather than only pass or fail. Issue #70; the fix is the
backport of upstream ESPHome PR #17800 in components/web_server_idf.

Without the fix, a client that stops reading has its session object freed while esp_http_server
still holds the pointer, **and the socket is never closed**. That last part is what this measures,
because it is the difference a client can see from outside:

  closed by the device   After about 20s without send progress the device gives up on a stalled
                         client. The fixed firmware shuts the socket down, so our end sees EOF. The
                         unfixed one only marks the session dead and leaves the socket open, so our
                         end sees nothing.
  session slots          A held-open stalled client occupies one. The device tracks about seven
                         event-source sessions, so this run stays under that: a client opened beyond
                         the limit gets a socket but never becomes a stalled session, and there is
                         then nothing for the device to close — which reads as a false "left open".
  restarts               /api/device/status carries uptime_s and reset_reason; uptime going
                         backwards means the corruption reached a panic.

Two things deliberately do *not* decide the verdict. Free heap wanders by ±10 KB on a live device, so
a per-cycle delta from it is noise — `--sample` is a context column only, and the monotone low-water
mark (`/sensor/Heap%20internal%20min` on a build with the stock `debug:` component) is the one worth
passing. And closing our own socket after each cycle lets the unfixed device finish the teardown and
hand the slot back, which hides the whole thing; that is why the sockets are held instead.

    # against the broken firmware — expected to find the bug
    scripts/diag/events_stall.py --base http://10.0.0.5 --auth admin:admin --expect broken

    # against the fixed one — expected to find it gone
    scripts/diag/events_stall.py --base http://10.0.0.5 --auth admin:admin --expect fixed

Either way the exit status is 0 when the observation matches --expect, so the pair of runs is the
proof. --out writes the run as JSON and --compare prints two runs side by side. A run that cannot
stall anything reports INCONCLUSIVE and never BROKEN.

Standard library only. It opens and closes /events and reads /api/device/status; it writes nothing to
the device and never touches an OTA route.
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
    """RFC 7616 MD5 qop=auth. ESPHome's digest is stateless, with a fresh nonce per challenge and no
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
    """JSON reads go through urllib, which answers digest challenges itself. A stalled /events client
    needs a small receive window and must not read its body, so it gets a connection whose socket is
    built by hand — SO_RCVBUF has to be set before connect() to shrink the window the device is told
    about."""

    def __init__(self, base: str, auth: str | None, timeout: float):
        url = urlparse(base)
        self.host, self.port = url.hostname, url.port or 80
        self.base = f"http://{self.host}:{self.port}"
        self.timeout = timeout
        self.user, _, self.password = (auth or "").partition(":")
        self.challenge: dict[str, str] = {}
        # An empty ProxyHandler suppresses the default one, which would otherwise send every request
        # to whatever HTTP_PROXY happens to be set in the environment.
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

    def sensor(self, path: str | None) -> float | None:
        if not path:
            return None
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
            # A small receive window is what lets the device wedge mid-send: its connect burst is the
            # config event, one per sorting group, then every entity.
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
            auth = None
            if self.user and self.challenge:
                auth = _digest_response(self.user, self.password, path, self.challenge)
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
            response.read()
            conn.close()
            if not self.refresh_challenge(path):
                return None, 401
        return None, 401


def closed_by_device(
    conn: http.client.HTTPConnection, drain_timeout: float
) -> bool | None:
    """Whether the device shut our stalled socket down itself.

    Drains whatever is buffered and then looks for EOF. The fixed firmware closes the session through
    HTTPD, so EOF arrives; the unfixed one only marks it dead and the socket stays open, so the read
    times out instead. None means the socket was already gone for some other reason.
    """
    sock = conn.sock
    if sock is None:
        return None
    sock.settimeout(drain_timeout)
    try:
        while True:
            if not sock.recv(4096):
                return True  # EOF: the device closed it
    except TimeoutError:
        return False  # still open once everything buffered had been read
    except OSError:
        return True  # a reset is the device tearing it down too


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

    record = {
        "cycles_requested": args.cycles,
        "hold_s": args.hold,
        "uptime_at_start_s": first.get("uptime_s"),
        "reset_reason_at_start": first.get("reset_reason"),
        "sample_path": args.sample,
        "baseline_sample": device.sensor(args.sample),
        "cycles": [],
    }

    print(
        f"uptime at start {first.get('uptime_s')}s, reset_reason {first.get('reset_reason')!r}"
    )
    if record["baseline_sample"] is not None:
        print(f"{args.sample} at start: {record['baseline_sample']:.0f}")
    print(
        f"\nStalling {args.cycles} clients, holding every one open, {args.hold:.0f}s each\n"
    )

    held: list = []
    previous_uptime = first.get("uptime_s") or 0
    try:
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
            held.append(stalled)
            # Hold without reading: the window closes and the device eventually gives up on it.
            time.sleep(args.hold)

            # Deliberately NOT inspected here: reading a stalled socket opens its receive
            # window, the device resumes sending and the stall timer resets — the check
            # would destroy what it measures. Every held socket is judged after the run.
            probe, probe_status = device.open_events(args.path, None)
            if probe is not None:
                probe.close()
            state = device.status()
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
                    "sample": device.sensor(args.sample),
                }
            )
            line = f"cycle {cycle:2}: stalled and held   probe /events -> {probe_status or 'no answer'}"
            if rebooted:
                line += f"  *** REBOOT, reset_reason={state.get('reset_reason')!r} ***"
            print(line)
            if rebooted and args.stop_on_reboot:
                print(
                    "stopping: the device restarted, which is one of the failures this looks for"
                )
                break
    finally:
        # The device times sessions out in the order they stalled, so a per-cycle look at the newest
        # socket sees an interleaving, not an answer. What matters is whether it closed them all, so
        # every held socket is judged here, once the run is over.
        record["closed_at_end"] = 0
        record["open_at_end"] = 0
        for conn in held:
            state = closed_by_device(conn, args.drain_timeout)
            if state is False:
                record["open_at_end"] += 1
            else:
                record["closed_at_end"] += 1
            try:
                conn.close()
            except OSError:
                pass
        if held:
            print(
                f"\nafter the run: the device had closed {record['closed_at_end']} of {len(held)} "
                f"stalled sockets, {record['open_at_end']} still open"
            )

    record["final_sample"] = device.sensor(args.sample)
    return verdict(record)


def verdict(record: dict) -> dict:
    cycles = record.get("cycles", [])
    completed = [c for c in cycles if c.get("stall_opened")]
    reboots = [c for c in completed if c.get("rebooted")]
    # Only a probe taken after a client really was stalled says anything about session slots.
    refused = [c for c in completed if c.get("probe_status") != 200]
    # Per-cycle counts are kept for the log, but the verdict uses the end-of-run sweep.
    left_open = record.get("open_at_end", 0)
    shut = record.get("closed_at_end", 0)

    record["completed_cycles"] = len(completed)
    record["reboots"] = len(reboots)
    record["slots_lost_at_cycle"] = refused[0]["cycle"] if refused else None
    record["stalling_failed_at_cycle"] = next(
        (c["cycle"] for c in cycles if not c.get("stall_opened")), None
    )
    record["left_open"] = left_open
    record["device_closed"] = shut
    if (
        record.get("baseline_sample") is not None
        and record.get("final_sample") is not None
    ):
        record["sample_delta"] = record["final_sample"] - record["baseline_sample"]

    if not completed:
        record["state"] = "inconclusive"
    elif reboots or record["slots_lost_at_cycle"] is not None or left_open:
        record["state"] = "broken"
    elif shut and len(completed) == record["cycles_requested"]:
        record["state"] = "fixed"
    else:
        record["state"] = "inconclusive"
    return record


def report(record: dict) -> None:
    print("\n--- what this run saw ---")
    print(
        f"clients actually stalled:                 {record['completed_cycles']}/{record['cycles_requested']}"
    )
    print(f"stalled sockets the device closed itself: {record['device_closed']}")
    print(f"stalled sockets left open:                {record['left_open']}")
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
    print(f"device restarts during the run:           {record['reboots']}")
    if "sample_delta" in record:
        print(
            f"{record['sample_path']}: {record['baseline_sample']:.0f} -> "
            f"{record['final_sample']:.0f} ({record['sample_delta']:+.0f}) — context, not a verdict"
        )

    state = record["state"]
    if state == "broken":
        print(
            "\nBROKEN: the device left stalled sockets open, lost session slots, or restarted."
            "\nA socket it never closes is a session object it freed too early."
        )
    elif state == "fixed":
        print(
            "\nFIXED: the device closed every stalled session itself and kept serving new ones."
        )
    else:
        print(
            "\nINCONCLUSIVE: not enough clients were stalled to conclude anything. Check the"
            "\ncredentials, and that --hold is longer than the device's 20s stall timeout."
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
            "closed by the device",
            before.get("device_closed", "-"),
            after.get("device_closed", "-"),
        ),
        ("left open", before.get("left_open", "-"), after.get("left_open", "-")),
        (
            "slots lost at cycle",
            before["slots_lost_at_cycle"] or "-",
            after["slots_lost_at_cycle"] or "-",
        ),
        ("device restarts", before["reboots"], after["reboots"]),
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
        default=9,
        help="stalled clients to open and hold; there are 7 slots",
    )
    p.add_argument(
        "--hold",
        type=float,
        default=45.0,
        help="seconds to hold each stalled client before judging it. The device gives up after about "
        "20s without send progress, so leave margin above that.",
    )
    p.add_argument(
        "--rcvbuf", type=int, default=2048, help="SO_RCVBUF; the kernel may round it up"
    )
    p.add_argument(
        "--drain-timeout",
        type=float,
        default=3.0,
        help="seconds to wait for EOF on a stalled socket",
    )
    p.add_argument(
        "--sample",
        help="a numeric REST getter for a context column. Prefer the monotone low-water mark, e.g. "
        "/sensor/Heap%%20internal%%20min — free heap wanders and proves nothing.",
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
