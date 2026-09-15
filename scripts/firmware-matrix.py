#!/usr/bin/env python3
"""Emit GitHub Actions build matrices from firmwares.yaml.

Usage:
    scripts/firmware-matrix.py build     # every firmware, for compile jobs
    scripts/firmware-matrix.py upload    # the upload: true subset, for publish jobs

Prints a JSON object shaped for `strategy.matrix`, e.g. {"include": [...]}.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import yaml

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = REPO_ROOT / "firmwares.yaml"


def entries() -> list[dict]:
    try:
        data = yaml.safe_load(MANIFEST.read_text())
    except FileNotFoundError:
        raise SystemExit(f"{MANIFEST.relative_to(REPO_ROOT)} not found")
    items = data.get("firmwares") if isinstance(data, dict) else None
    if not items:
        raise SystemExit("firmwares.yaml: no `firmwares:` entries")

    result = []
    for item in items:
        config = item.get("config")
        device = item.get("device")
        upload = item.get("upload", False)
        if not config or not isinstance(config, str):
            raise SystemExit(f"firmwares.yaml: entry without `config`: {item}")
        if not device or not isinstance(device, str):
            raise SystemExit(f"firmwares.yaml: {config}: missing `device` slug")
        if not isinstance(upload, bool):
            raise SystemExit(f"firmwares.yaml: {config}: `upload` must be a boolean")
        if not (REPO_ROOT / config).is_file():
            raise SystemExit(f"firmwares.yaml: {config}: file not found")
        result.append(
            {
                "config": config,
                # Build stem, artifact name: the basename, since configs live
                # under devices/<family>/.
                "name": Path(config).stem,
                "device": device,
                "upload": upload,
            }
        )

    names = [item["name"] for item in result]
    if len(names) != len(set(names)):
        raise SystemExit("firmwares.yaml: duplicate config entries")

    # Two uploaded entries on one device would overwrite each other on the
    # firmware server: same hierarchy level, same version, force_overwrite on.
    # The server lowercases slugs when generating the hierarchy, so the check
    # is case-insensitive to match it.
    uploaded = [item["device"].lower() for item in result if item["upload"]]
    if len(uploaded) != len(set(uploaded)):
        raise SystemExit(
            "firmwares.yaml: duplicate device among upload: true entries — "
            "they would overwrite each other on the firmware server"
        )
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "mode",
        choices=("build", "upload"),
        help="build: every firmware; upload: only the upload: true subset",
    )
    args = parser.parse_args()

    items = entries()
    if args.mode == "upload":
        items = [item for item in items if item["upload"]]

    include = [
        {key: item[key] for key in ("config", "name", "device")} for item in items
    ]
    print(json.dumps({"include": include}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
