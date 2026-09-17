#!/usr/bin/env python3
"""Compute the next release-channel firmware version.

The release channel versions firmware as <esphome>.<sub>, where <esphome> is
the pin in requirements.txt and <sub> is one more than the highest subversion
tagged so far (0 when none exists). Both the Release workflow's resolve step
and the draft-release workflow call this script, so the number they compute
cannot drift apart.

Usage:
    git tag --list | scripts/release-version.py     # next version
    scripts/release-version.py --pin                # just the esphome pin

Tags are read from stdin, one per line.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

REPO_ROOT = Path(__file__).resolve().parent.parent

PIN_RE = re.compile(r"^esphome==([0-9]+(?:\.[0-9]+)+)$")


def esphome_pin() -> str:
    for line in (REPO_ROOT / "requirements.txt").read_text().splitlines():
        if match := PIN_RE.match(line.strip()):
            return match.group(1)
    raise SystemExit("cannot parse the esphome pin from requirements.txt")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--pin",
        action="store_true",
        help="print only the esphome pin, no tag list needed",
    )
    args = parser.parse_args()

    pin = esphome_pin()
    if args.pin:
        print(pin)
        return 0

    sub_re = re.compile(rf"^{re.escape(pin)}\.(\d+)$")
    highest = -1
    for line in sys.stdin:
        if match := sub_re.match(line.strip()):
            highest = max(highest, int(match.group(1)))
    print(f"{pin}.{highest + 1}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
