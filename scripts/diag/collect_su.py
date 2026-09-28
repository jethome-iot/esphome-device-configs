#!/usr/bin/env python3
"""Collect every GCC -fstack-usage .su file from the IDF build into one corpus.

Emits stack_usage.csv (file,line,col,function,bytes,qualifier,su_file,component)
and stack_usage.json.  Run after a build made with
  EXTRA_CFLAGS=-fstack-usage EXTRA_CXXFLAGS=-fstack-usage esphome compile <yaml>
"""

import csv
import json
import re
import sys
from pathlib import Path

BUILD = Path(sys.argv[1])
OUT = Path(sys.argv[2])

LOC = re.compile(r"^(?P<file>.+?):(?P<line>\d+):(?P<col>\d+):(?P<func>.*)$", re.S)
# [bootloader/]esp-idf/<comp path>/CMakeFiles/<lib>.dir/<relpath>.su
COMP = re.compile(r"^(?:(bootloader)/)?esp-idf/(.+?)/CMakeFiles/[^/]+\.dir/(.*)\.su$")

rows, bad = [], []
for su in sorted(BUILD.rglob("*.su")):
    rel = su.relative_to(BUILD).as_posix()
    m = COMP.match(rel)
    if m:
        image = "bootloader" if m.group(1) else "app"
        comp_path = m.group(2)
        component = comp_path.split("/")[0]
        tu = m.group(3)
    else:
        image, comp_path, component, tu = "?", rel, rel.split("/")[0], rel
    text = su.read_text(encoding="utf-8", errors="replace")
    # A .su record is one line, but GCC can emit an embedded newline inside a
    # pretty-printed name; records are re-joined on the "<n>\t<qual>" tail.
    for rec in re.finditer(
        r"(?m)^(.*?)\t(\d+)\t(static|dynamic|bounded|dynamic,bounded)$", text, re.S
    ):
        loc, nbytes, qual = rec.group(1), int(rec.group(2)), rec.group(3)
        lm = LOC.match(loc)
        if not lm:
            bad.append((rel, loc))
            continue
        rows.append(
            {
                "image": image,
                "component": component,
                "comp_path": comp_path,
                "tu": tu,
                "su_file": rel,
                "file": lm.group("file"),
                "line": int(lm.group("line")),
                "col": int(lm.group("col")),
                "function": " ".join(lm.group("func").split()),
                "bytes": nbytes,
                "qualifier": qual,
            }
        )

OUT.mkdir(parents=True, exist_ok=True)
fields = [
    "image",
    "component",
    "comp_path",
    "tu",
    "file",
    "line",
    "col",
    "function",
    "bytes",
    "qualifier",
    "su_file",
]
with (OUT / "stack_usage.csv").open("w", newline="", encoding="utf-8") as f:
    w = csv.DictWriter(f, fieldnames=fields)
    w.writeheader()
    for r in sorted(rows, key=lambda r: -r["bytes"]):
        w.writerow({k: r[k] for k in fields})
(OUT / "stack_usage.json").write_text(json.dumps(rows, indent=1), encoding="utf-8")
print(
    f"su files: {len(list(BUILD.rglob('*.su')))}  entries: {len(rows)}  unparsed: {len(bad)}"
)
for b in bad[:10]:
    print("  BAD", b)
