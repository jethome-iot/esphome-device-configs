#!/usr/bin/env python3
"""Show that a deep /files/copy used to panic the device, and that it is now refused instead.

Written to be run twice — against the firmware with the old recursion cap and against the one with
the new — and to say which of the two it is looking at. Issues #72 and #64, part of #60.

What breaks without the fix: `delete` and `copy` recurse on the ESP-IDF httpd task, which has 4352
bytes of stack, and hanging off every `remove()` is a LittleFS -> flash -> cache-disable ->
cross-core-IPC tail of about 1744 bytes that does not scale with directory depth. The old cap of 8
counted only the walk's own frames, so a six-level copy overran the stack and the canary watchpoint
panicked the device. The new cap refuses the request instead.

The failure is a restart, so that is what this detects: /api/device/status carries uptime_s and
reset_reason, and uptime going backwards means the device went down. It walks the depth up one level
at a time and stops at the first restart.

    # against the old firmware — expected to find the panic
    scripts/diag/copy_depth.py --base http://10.0.0.5 --auth admin:admin --expect broken

    # against the fixed one — expected to find a clean refusal and no restart
    scripts/diag/copy_depth.py --base http://10.0.0.5 --auth admin:admin --expect fixed

Either way the exit status is 0 when the observation matches --expect, so the pair of runs is the
proof. --out writes the run as JSON and --compare prints two runs side by side.

**This writes to the device**: it creates directories and a small file under the mount, copies them,
and removes both trees afterwards. Run it against a test board, not a unit in service — and on a
firmware with the old cap it will panic that board on purpose. It touches only /files routes and
/api/device/status, never an OTA route.

Note on the emulator: QEMU completes depth 10 on the same code because it never takes the cross-core
tail, so a run there proves nothing about the stack. Hardware is the only gate.
"""

import argparse
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

STATUS_PATH = "/api/device/status"


class Device:
    def __init__(self, base: str, auth: str | None, prefix: str, timeout: float):
        self.base = base.rstrip("/")
        self.prefix = "/" + prefix.strip("/")
        self.timeout = timeout
        # An empty ProxyHandler suppresses the default one, which would otherwise route every
        # request through whatever HTTP_PROXY is set in the environment.
        handlers: list = [urllib.request.ProxyHandler({})]
        if auth:
            user, _, password = auth.partition(":")
            mgr = urllib.request.HTTPPasswordMgrWithDefaultRealm()
            mgr.add_password(None, self.base, user, password)
            handlers += [
                urllib.request.HTTPDigestAuthHandler(mgr),
                urllib.request.HTTPBasicAuthHandler(mgr),
            ]
        self.opener = urllib.request.build_opener(*handlers)

    def _call(
        self, method: str, path: str, params: dict[str, str], body: bytes | None = None
    ) -> int:
        """Returns the HTTP status, or 0 when the device did not answer at all."""
        url = (
            f"{self.base}{path}?{urllib.parse.urlencode(params)}"
            if params
            else self.base + path
        )
        request = urllib.request.Request(url, data=body, method=method)
        try:
            with self.opener.open(request, timeout=self.timeout) as response:
                response.read()
                return response.status
        except urllib.error.HTTPError as e:
            e.read()
            return e.code
        except (urllib.error.URLError, OSError):
            return 0

    def mkdir(self, path: str) -> int:
        return self._call("POST", self.prefix + "/mkdir", {"path": path})

    def write(self, path: str, content: bytes) -> int:
        return self._call("POST", self.prefix + "/write", {"path": path}, content)

    def copy(self, old: str, new: str) -> int:
        return self._call(
            "POST", self.prefix + "/copy", {"old_path": old, "new_path": new}
        )

    def delete(self, path: str) -> int:
        return self._call("POST", self.prefix + "/delete", {"path": path})

    def status(self) -> dict:
        try:
            with self.opener.open(
                self.base + STATUS_PATH, timeout=self.timeout
            ) as response:
                return json.loads(response.read())
        except (urllib.error.URLError, OSError, ValueError):
            return {}

    def wait_until_up(self, seconds: float) -> dict:
        """After a restart the device takes a while to serve again."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            state = self.status()
            if state:
                return state
            time.sleep(2.0)
        return {}


def levels(root: str, depth: int) -> list[str]:
    """root/L1, root/L1/L2, ... — depth directories, the named one counting as the first."""
    paths, path = [], root
    for i in range(1, depth + 1):
        path = f"{path}/L{i}" if i > 1 else root
        paths.append(path)
    return paths


def attempt(device: Device, root: str, dest: str, depth: int) -> dict:
    """Builds a tree `depth` levels deep, copies it, removes both. Returns what happened."""
    made = []
    for path in levels(root, depth):
        status = device.mkdir(path)
        if status not in (200, 201):
            return {"depth": depth, "stage": "mkdir", "status": status}
        made.append(path)
    if made:
        device.write(made[-1] + "/f.txt", b"q")

    copy_status = device.copy(root, dest)
    return {"depth": depth, "stage": "copy", "status": copy_status}


def run(device: Device, args) -> dict:
    first = device.status()
    if not first:
        print(f"Cannot read {STATUS_PATH} — check --base and --auth.", file=sys.stderr)
        return {}

    root = f"{args.dir}/D"
    dest = f"{args.dir}/C"
    record = {
        "max_depth": args.max_depth,
        "uptime_at_start_s": first.get("uptime_s"),
        "reset_reason_at_start": first.get("reset_reason"),
        "attempts": [],
    }
    print(
        f"uptime at start {first.get('uptime_s')}s, reset_reason {first.get('reset_reason')!r}\n"
    )

    previous_uptime = first.get("uptime_s") or 0
    for depth in range(1, args.max_depth + 1):
        device.delete(root)
        device.delete(dest)
        result = attempt(device, root, dest, depth)

        state = device.status() or device.wait_until_up(args.recover)
        uptime = state.get("uptime_s")
        rebooted = uptime is not None and uptime < previous_uptime
        unreachable = not state
        if uptime is not None:
            previous_uptime = uptime

        result.update(
            {
                "uptime_s": uptime,
                "reset_reason": state.get("reset_reason"),
                "rebooted": rebooted,
                "unreachable": unreachable,
            }
        )
        record["attempts"].append(result)

        verdict_word = {200: "copied", 400: "refused", 0: "no answer"}.get(
            result["status"], str(result["status"])
        )
        line = f"depth {depth:2}: {result['stage']:<6} -> {verdict_word:<10} uptime {uptime}"
        if rebooted:
            line += f"  *** RESTARTED, reset_reason={state.get('reset_reason')!r} ***"
        if unreachable:
            line += "  *** did not come back within --recover ***"
        print(line)

        if rebooted or unreachable:
            break

    device.delete(root)
    device.delete(dest)
    return verdict(record)


def verdict(record: dict) -> dict:
    attempts = record["attempts"]
    crashed = [a for a in attempts if a["rebooted"] or a["unreachable"]]
    refused = [a for a in attempts if a["stage"] == "copy" and a["status"] == 400]
    copied = [a for a in attempts if a["stage"] == "copy" and a["status"] == 200]

    record["deepest_copied"] = max((a["depth"] for a in copied), default=None)
    record["shallowest_refused"] = min((a["depth"] for a in refused), default=None)
    record["crashed_at_depth"] = crashed[0]["depth"] if crashed else None

    if crashed:
        record["state"] = "broken"
    elif refused:
        record["state"] = "fixed"
    else:
        record["state"] = "inconclusive"
    return record


def report(record: dict) -> None:
    print("\n--- what this run saw ---")
    print(f"deepest tree copied:        {record['deepest_copied']}")
    print(f"shallowest tree refused:    {record['shallowest_refused']}")
    print(f"device went down at depth:  {record['crashed_at_depth']}")

    state = record["state"]
    if state == "broken":
        print("\nBROKEN: a copy took the device down instead of being refused.")
    elif state == "fixed":
        print(
            "\nFIXED: the copy is refused above the cap and the device stayed up throughout."
        )
    else:
        print(
            "\nINCONCLUSIVE: nothing was refused and nothing crashed — raise --max-depth."
        )


def compare(before_path: str, after_path: str) -> int:
    with open(before_path) as fh:
        before = json.load(fh)
    with open(after_path) as fh:
        after = json.load(fh)
    rows = [
        ("state", before["state"], after["state"]),
        ("deepest copied", before["deepest_copied"], after["deepest_copied"]),
        (
            "shallowest refused",
            before["shallowest_refused"] or "-",
            after["shallowest_refused"] or "-",
        ),
        (
            "went down at depth",
            before["crashed_at_depth"] or "-",
            after["crashed_at_depth"] or "-",
        ),
    ]
    width = max(len(r[0]) for r in rows)
    print(f"{'':<{width}}  {'before':>16}  {'after':>16}")
    for name, b, a in rows:
        print(f"{name:<{width}}  {str(b):>16}  {str(a):>16}")
    if before["state"] == "broken" and after["state"] == "fixed":
        print("\nThe fix does what it claims: it panicked before, it refuses now.")
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
    p.add_argument("--prefix", default="files", help="the file API's url_prefix")
    p.add_argument(
        "--dir", default="/depth", help="where under the mount to build the trees"
    )
    p.add_argument(
        "--max-depth", type=int, default=8, help="how deep to walk before giving up"
    )
    p.add_argument(
        "--recover",
        type=float,
        default=90.0,
        help="seconds to wait for a restarted device",
    )
    p.add_argument(
        "--expect",
        choices=["broken", "fixed"],
        help="exit non-zero unless the run sees this",
    )
    p.add_argument("--out", help="write the run to this file as JSON")
    p.add_argument(
        "--compare",
        nargs=2,
        metavar=("BEFORE", "AFTER"),
        help="compare two --out files and exit",
    )
    p.add_argument("--timeout", type=float, default=20.0)
    args = p.parse_args()

    if args.compare:
        return compare(*args.compare)
    if not args.base:
        p.error("--base is required unless --compare is given")

    record = run(Device(args.base, args.auth, args.prefix, args.timeout), args)
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
