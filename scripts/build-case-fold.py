#!/usr/bin/env python3
"""Build the case-fold table names are compared with, the device's copy and the dashboard's.

A capital of Latin, Greek or Cyrillic folds to its simple case folding (Unicode's CaseFolding,
statuses C and S), from the Unicode data of the Python that runs this. A capital is left out when
it folds to several characters, into ASCII or into another script: the device must not fold a
name further than these few blocks reach, and the browser, which knows all of Unicode, must fold
it no further either.

Writes:
    components/case_fold/case_fold_table.h          the device's table
    components/case_fold/client/caseFoldTable.ts    the dashboard's table
    tests/components/case_fold/fold_cases.tsv       every code point the table folds, and some
                                                    it must not; both sides are held to it

Usage:
    scripts/build-case-fold.py            # regenerate
    scripts/build-case-fold.py --check    # fail if any of them is stale
"""

from __future__ import annotations

import argparse
from pathlib import Path
import sys
import unicodedata

REPO_ROOT = Path(__file__).resolve().parent.parent
TABLE_H = REPO_ROOT / "components" / "case_fold" / "case_fold_table.h"
TABLE_TS = REPO_ROOT / "components" / "case_fold" / "client" / "caseFoldTable.ts"
FIXTURE = REPO_ROOT / "tests" / "components" / "case_fold" / "fold_cases.tsv"

# Where the capitals that fold are: A-Z, and the blocks a name in these scripts is written in.
BLOCKS = [
    (0x0041, 0x005A),  # Basic Latin, A-Z only
    (0x0080, 0x00FF),  # Latin-1 Supplement
    (0x0100, 0x017F),  # Latin Extended-A
    (0x0180, 0x024F),  # Latin Extended-B
    (0x0370, 0x03FF),  # Greek and Coptic
    (0x0400, 0x04FF),  # Cyrillic
    (0x0500, 0x052F),  # Cyrillic Supplement
    (0x1E00, 0x1EFF),  # Latin Extended Additional
]

# Outside the blocks, each with a fold Unicode knows and the table must not: what a browser's
# toLowerCase() would fold and the device would not.
OUTSIDE = [
    0x0345,  # COMBINING GREEK YPOGEGRAMMENI
    0x0531,  # ARMENIAN CAPITAL LETTER AYB
    0x10A0,  # GEORGIAN CAPITAL LETTER AN
    0x1C80,  # CYRILLIC SMALL LETTER ROUNDED VE, Cyrillic Extended-C
    0x1F08,  # GREEK CAPITAL LETTER ALPHA WITH PSILI, Greek Extended
    0x2126,  # OHM SIGN
    0x212A,  # KELVIN SIGN
    0x212B,  # ANGSTROM SIGN
    0x2160,  # ROMAN NUMERAL ONE
    0x24B6,  # CIRCLED LATIN CAPITAL LETTER A
    0x2C62,  # LATIN CAPITAL LETTER L WITH MIDDLE TILDE, Latin Extended-C
    0xA640,  # CYRILLIC CAPITAL LETTER ZEMLYA, Cyrillic Extended-B
    0xA722,  # LATIN CAPITAL LETTER EGYPTOLOGICAL ALEF, Latin Extended-D
    0xFF21,  # FULLWIDTH LATIN CAPITAL LETTER A
    0x10400,  # DESERET CAPITAL LETTER LONG I
]

# Inside the blocks and folding to nothing else: the gaps between capitals, and letters a reader
# might expect to fold.
UNFOLDED = [
    0x00D7,  # MULTIPLICATION SIGN, between the Latin-1 capitals
    0x0131,  # LATIN SMALL LETTER DOTLESS I: I folds to i, and nothing folds to it
    0x0138,  # LATIN SMALL LETTER KRA
    0x03C3,  # GREEK SMALL LETTER SIGMA: final sigma folds to it
    0x0430,  # CYRILLIC SMALL LETTER A
    0x04CF,  # CYRILLIC SMALL LETTER PALOCHKA: its capital folds to it
]


def simple_fold(code_point: int) -> int | None:
    """The simple case folding, C or S, or None when there is only a full one."""
    char = chr(code_point)
    folded = char.casefold()
    if len(folded) == 1:
        return ord(folded)
    # S: a capital whose full folding has several characters, its lowercase one.
    lower = char.lower()
    if len(lower) == 1 and lower != char and lower.casefold() == folded:
        return ord(lower)
    return None


def script(code_point: int) -> str:
    return unicodedata.name(chr(code_point)).split()[0]


def why_unfolded(code_point: int) -> str | None:
    """Why a capital of the blocks stays as it is, or None when it folds."""
    folded = simple_fold(code_point)
    if folded is None:
        return "folds to several characters"
    if folded == code_point:
        return None
    if code_point >= 0x80 and folded < 0x80:
        return "folds into ASCII"
    if script(code_point) != script(folded):
        return "folds into another script"
    return None


def pairs() -> list[tuple[int, int]]:
    out = []
    for first, last in BLOCKS:
        for code_point in range(first, last + 1):
            folded = simple_fold(code_point)
            if folded != code_point and why_unfolded(code_point) is None:
                out.append((code_point, folded))
    return out


def kept() -> list[tuple[int, str]]:
    """Code points the table must leave alone, with why."""
    out = []
    for first, last in BLOCKS:
        for code_point in range(first, last + 1):
            reason = why_unfolded(code_point)
            if reason is not None:
                out.append((code_point, reason))
    out += [(code_point, "outside the folded blocks") for code_point in OUTSIDE]
    out += [(code_point, "folds to itself") for code_point in UNFOLDED]
    return sorted(out)


def ranges(folds: list[tuple[int, int]]) -> list[tuple[int, int, int, int]]:
    """(first, last, delta, stride): runs of capitals one or two apart that fold by one delta."""
    out: list[tuple[int, int, int, int]] = []
    for code_point, folded in folds:
        delta = folded - code_point
        if out:
            first, last, last_delta, stride = out[-1]
            step = code_point - last
            if last_delta == delta and (
                step == stride or (first == last and step in (1, 2))
            ):
                out[-1] = (first, code_point, delta, step)
                continue
        out.append((code_point, code_point, delta, 1))
    return out


def check(folds: list[tuple[int, int]], rows: list[tuple[int, int, int, int]]) -> None:
    """What the C++ lookup relies on: sorted, no overlap, 16-bit fields, the table's own pairs."""
    covered = {}
    previous_last = -1
    for first, last, delta, stride in rows:
        assert previous_last < first <= last <= 0xFFFF, hex(first)
        assert -0x8000 <= delta <= 0x7FFF and stride in (1, 2), hex(first)
        previous_last = last
        for code_point in range(first, last + 1, stride):
            covered[code_point] = code_point + delta
    assert covered == dict(folds)
    assert all(
        0x80 <= folded <= 0xFFFF or code_point < 0x80 for code_point, folded in folds
    )
    sources = {code_point for code_point, _ in folds}
    assert not any(folded in sources for _, folded in folds), "a fold folds again"


def hex4(code_point: int) -> str:
    return f"0x{code_point:04X}"


def table_h(rows: list[tuple[int, int, int, int]]) -> str:
    lines = [
        "// Generated by scripts/build-case-fold.py: do not edit.",
        "#pragma once",
        "",
        "#include <cstdint>",
        "",
        "namespace esphome::case_fold {",
        "",
        "// Code points first..last fold to code point + delta: every one of them for a stride of 1,",
        "// every other one from first for a stride of 2. Sorted by first, none overlapping.",
        "struct FoldRange {",
        "  uint16_t first;",
        "  uint16_t last;",
        "  int16_t delta;",
        "  uint8_t stride;",
        "};",
        "",
        "// One range a line, which clang-format would pack.",
        "// clang-format off",
        "inline constexpr FoldRange FOLD_RANGES[] = {",
    ]
    lines += [
        f"    {{{hex4(first)}, {hex4(last)}, {delta}, {stride}}},"
        for first, last, delta, stride in rows
    ]
    lines += ["};", "// clang-format on", "", "}  // namespace esphome::case_fold", ""]
    return "\n".join(lines)


def table_ts(rows: list[tuple[int, int, int, int]]) -> str:
    lines = [
        "// Generated by scripts/build-case-fold.py: do not edit.",
        "",
        "/**",
        " * [first, last, delta, stride]: code points first..last fold to code point + delta, every one",
        " * of them for a stride of 1, every other one from first for a stride of 2. Sorted by first,",
        " * none overlapping.",
        " */",
        "export const FOLD_RANGES: ReadonlyArray<readonly [number, number, number, number]> = [",
    ]
    lines += [
        f"  [{hex4(first)}, {hex4(last)}, {delta}, {stride}],"
        for first, last, delta, stride in rows
    ]
    lines += ["]", ""]
    return "\n".join(lines)


def fixture(folds: list[tuple[int, int]], unfolded: list[tuple[int, str]]) -> str:
    lines = [
        "# Generated by scripts/build-case-fold.py: do not edit.",
        "# A code point, what it folds to, and for one that stays, why. Every code point the table",
        "# folds is here; any other folds to itself.",
    ]
    rows = [
        (code_point, f"{code_point:04X}\t{folded:04X}") for code_point, folded in folds
    ]
    rows += [
        (
            code_point,
            f"{code_point:04X}\t{code_point:04X}\t{unicodedata.name(chr(code_point))}: {reason}",
        )
        for code_point, reason in unfolded
    ]
    lines += [line for _, line in sorted(rows)]
    return "\n".join(lines) + "\n"


def outputs() -> dict[Path, str]:
    folds = pairs()
    rows = ranges(folds)
    check(folds, rows)
    return {
        TABLE_H: table_h(rows),
        TABLE_TS: table_ts(rows),
        FIXTURE: fixture(folds, kept()),
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        "--check", action="store_true", help="fail if a generated file is stale"
    )
    args = parser.parse_args()

    stale = []
    for path, text in outputs().items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == text:
            continue
        stale.append(path.relative_to(REPO_ROOT))
        if not args.check:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
    if args.check and stale:
        print(
            "stale, run scripts/build-case-fold.py: " + ", ".join(map(str, stale)),
            file=sys.stderr,
        )
        return 1
    for path in stale:
        print(f"wrote {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
