#!/usr/bin/env python3
"""Show what components/ changed in the ESPHome components it shadows.

A component here whose name also exists in the installed ESPHome replaces that one at
build time, so a version bump does not reach it: it has to be re-copied and re-patched
by hand. The installed copy is the pinned upstream, which makes this diff the patch to
re-apply, and every hunk of it is marked `JetHome:` in the source.

--check reads that the other way round: a hunk carrying no marker is either a change
somebody forgot to mark or upstream moving on without the copy, and it fails. It is a
smoke alarm, not a proof — a stray line within three of a marker shares that hunk and
rides along.

Usage:
    scripts/vendored-diff.py                     # every shadowing component
    scripts/vendored-diff.py display_menu_base   # just this one
    scripts/vendored-diff.py --check             # fail on a hunk with no JetHome marker
"""

from __future__ import annotations

import argparse
import difflib
from pathlib import Path
import sys

import esphome.const

REPO_ROOT = Path(__file__).resolve().parent.parent
OURS = REPO_ROOT / "components"
UPSTREAM = Path(esphome.__file__).resolve().parent / "components"
SUFFIXES = (".py", ".h", ".cpp")
MARKER = "JetHome"


def shadowing() -> list[str]:
    return sorted(
        d.name for d in OURS.iterdir() if d.is_dir() and (UPSTREAM / d.name).is_dir()
    )


def sources(root: Path) -> set[Path]:
    """Every source file under root, relative to it, platform subdirectories included."""
    return {
        p.relative_to(root)
        for p in root.rglob("*")
        if p.suffix in SUFFIXES and "__pycache__" not in p.parts
    }


def diff_file(name: str, rel: Path) -> list[str]:
    theirs, ours = UPSTREAM / name / rel, OURS / name / rel
    return list(
        difflib.unified_diff(
            theirs.read_text().splitlines(keepends=True) if theirs.is_file() else [],
            ours.read_text().splitlines(keepends=True) if ours.is_file() else [],
            fromfile=f"esphome {esphome.const.__version__}/{name}/{rel}",
            tofile=f"components/{name}/{rel}",
        )
    )


def unmarked_hunks(name: str, rel: Path, lines: list[str]) -> list[str]:
    """The @@ headers of hunks that add nothing carrying the marker."""
    unmarked, header, marked = [], None, False
    for line in lines:
        if line.startswith("@@"):
            if header is not None and not marked:
                unmarked.append(f"{name}/{rel}: {header.rstrip()}")
            header, marked = line, False
        elif header is not None and line.startswith("+") and MARKER in line:
            marked = True
    if header is not None and not marked:
        unmarked.append(f"{name}/{rel}: {header.rstrip()}")
    return unmarked


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("components", nargs="*", help="default: all shadowing ones")
    parser.add_argument(
        "--check",
        action="store_true",
        help="print nothing; fail if a hunk carries no JetHome marker",
    )
    args = parser.parse_args()

    names = args.components or shadowing()
    if unknown := [
        n for n in names if not (UPSTREAM / n).is_dir() or not (OURS / n).is_dir()
    ]:
        print(f"not a component that components/ shadows: {', '.join(unknown)}")
        return 1
    if not names:
        print("components/ shadows no ESPHome component")
        return 0

    unmarked: list[str] = []
    for name in names:
        if not args.check:
            print(f"=== {name}")
        for rel in sorted(sources(OURS / name) | sources(UPSTREAM / name)):
            lines = diff_file(name, rel)
            if args.check:
                unmarked.extend(unmarked_hunks(name, rel, lines))
            else:
                sys.stdout.writelines(lines)

    if unmarked:
        print("Hunks with no JetHome marker:")
        print("\n".join(f"  {h}" for h in unmarked))
        print(
            f"\nEither mark them, or re-copy the component from esphome "
            f"{esphome.const.__version__} and re-apply the patch."
        )
        return 1
    if args.check:
        print(f"components/ is in step with esphome {esphome.const.__version__}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
