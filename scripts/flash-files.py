#!/usr/bin/env python3
"""Read the LittleFS user partition straight from flash, with the firmware stopped.

The board is read over its USB serial port with esptool, which puts it into its bootloader
and reboots it afterwards, so this reaches the files where scripts/device-files.py cannot: a
board with no network, or a firmware that does not boot. An image file works too: a dump
saved by `dump`, or a whole-flash image such as QEMU's qemu-flash.bin.

    scripts/flash-files.py --port /dev/ttyUSB0 dump littlefs.bin
    scripts/flash-files.py --image littlefs.bin ls
    scripts/flash-files.py --image littlefs.bin extract ./files
"""

from __future__ import annotations

import argparse
from collections.abc import Iterator
from dataclasses import dataclass
from datetime import datetime
import os
from pathlib import Path
import posixpath
import struct
import subprocess
import sys
import tempfile

try:
    from littlefs import LFSStat, LittleFS, LittleFSError, UserContext
except ImportError:
    sys.exit("littlefs-python is missing: pip install -r requirements-dev.txt")

TABLE_OFFSET = 0x8000
TABLE_SIZE = 0xC00
ENTRY = struct.Struct("<2sBBII16sI")
ENTRY_MAGIC = b"\xaa\x50"
# esp_littlefs uses the flash erase size as its block size.
BLOCK_SIZE = 4096
LITTLEFS_MAGIC = b"littlefs"
# esp_littlefs keeps a file's mtime in this attribute, as a little-endian time_t.
MTIME_ATTR = "t"
DEFAULT_PARTITION = "littlefs"
# Names are bytes to LittleFS, and the firmware takes any; latin-1 maps each byte to a
# character and back, and UTF-8 is applied only on the host side.
NAME_ENCODING = "latin-1"
WINDOWS = os.name == "nt"


class FlashError(Exception):
    """The image or the board does not hold what was asked for."""


@dataclass
class Partition:
    name: str
    offset: int
    size: int


def parse_partition_table(table: bytes) -> list[Partition]:
    partitions = []
    for pos in range(0, len(table) - ENTRY.size + 1, ENTRY.size):
        magic, _, _, offset, size, label, _ = ENTRY.unpack_from(table, pos)
        if magic != ENTRY_MAGIC:
            break  # the MD5 entry or the erased tail
        name = label.rstrip(b"\0").decode(errors="replace")
        partitions.append(Partition(name, offset, size))
    return partitions


def find_partition(table: bytes, name: str) -> Partition:
    partitions = parse_partition_table(table)
    if not partitions:
        raise FlashError(f"no partition table at {TABLE_OFFSET:#x}")
    for partition in partitions:
        if partition.name == name:
            return partition
    names = ", ".join(p.name for p in partitions)
    raise FlashError(f"no partition named {name!r}; the table has {names}")


def is_littlefs(image: bytes) -> bool:
    """The superblock, in either block of the pair, carries the magic at offset 8."""
    return any(
        image[block + 8 : block + 16] == LITTLEFS_MAGIC for block in (0, BLOCK_SIZE)
    )


def partition_from_image(path: Path, name: str | None) -> bytes:
    data = path.read_bytes()
    if is_littlefs(data):
        if name is not None:
            raise FlashError(
                f"{path} is a partition dump: it has no table to pick {name!r} from"
            )
        return data
    name = name or DEFAULT_PARTITION
    table = data[TABLE_OFFSET : TABLE_OFFSET + TABLE_SIZE]
    try:
        partition = find_partition(table, name)
    except FlashError as e:
        raise FlashError(f"{path}: not a LittleFS image, and {e}") from None
    end = partition.offset + partition.size
    if end > len(data):
        raise FlashError(f"{path}: {name!r} ends at {end:#x}, past the image's end")
    return data[partition.offset : end]


def esptool_read(port: str, baud: int, offset: int, size: int) -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / "flash.bin"
        cmd = [sys.executable, "-m", "esptool", "--port", port, "--baud", str(baud)]
        # Back to the firmware after every read, so nothing that fails leaves the board
        # sitting in its bootloader.
        cmd += ["--after", "hard-reset", "read-flash", hex(offset), hex(size), str(out)]
        # Keep esptool's progress off stdout, which carries the listing.
        if subprocess.run(cmd, stdout=sys.stderr).returncode != 0:
            raise FlashError(f"esptool could not read {size:#x} bytes at {offset:#x}")
        return out.read_bytes()


def partition_from_board(port: str, baud: int, name: str | None) -> bytes:
    table = esptool_read(port, baud, TABLE_OFFSET, TABLE_SIZE)
    partition = find_partition(table, name or DEFAULT_PARTITION)
    return esptool_read(port, baud, partition.offset, partition.size)


def mount(image: bytes) -> LittleFS:
    fs = LittleFS(
        context=UserContext(buffer=bytearray(image)),
        block_size=BLOCK_SIZE,
        block_count=0,  # from the superblock
        filename_encoding=NAME_ENCODING,
        mount=False,
    )
    try:
        fs.mount()
    except LittleFSError as e:
        if is_littlefs(image):
            raise FlashError(f"the LittleFS is damaged or cut short: {e}") from None
        raise FlashError(f"no LittleFS on the partition: {e}") from None
    size = fs.block_count * BLOCK_SIZE
    if size > len(image):
        raise FlashError(f"truncated: {len(image)} bytes of a {size}-byte filesystem")
    return fs


def host_name(path: str, errors: str) -> str:
    return path.encode(NAME_ENCODING).decode("utf-8", errors)


def walk(fs: LittleFS, top: str = "/") -> Iterator[tuple[str, LFSStat]]:
    """Every entry under top, sorted by name, each directory followed by what it holds."""
    for entry in sorted(fs.scandir(top), key=lambda e: e.name):
        path = posixpath.join(top, entry.name)
        yield path, entry
        if entry.type == LFSStat.TYPE_DIR:
            yield from walk(fs, path)


def mtime(fs: LittleFS, path: str) -> int | None:
    try:
        t = int.from_bytes(fs.getattr(path, MTIME_ATTR), "little", signed=True)
        # A value no date fits comes from a damaged attribute; the file is still worth reading.
        datetime.fromtimestamp(t)
    except (LittleFSError, ValueError, OverflowError, OSError):
        return None
    return t or None


def cmd_dump(image: bytes, args: argparse.Namespace) -> None:
    args.output.write_bytes(image)
    print(f"{len(image)} bytes to {args.output}")


def cmd_ls(image: bytes, args: argparse.Namespace) -> None:
    fs = mount(image)
    for path, entry in walk(fs):
        is_dir = entry.type == LFSStat.TYPE_DIR
        t = mtime(fs, path)
        when = datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S") if t else ""
        size = "" if is_dir else entry.size
        name = host_name(path, "replace") + ("/" if is_dir else "")
        print(f"{size:>9}  {when:19}  {name}")


def extract_one(fs: LittleFS, path: str, entry: LFSStat, dest: Path) -> None:
    name = host_name(path, "surrogateescape")
    target = dest / name.lstrip("/")
    # On Windows, \ or : in a name would land outside dest or in an NTFS stream.
    if (WINDOWS and ":" in name) or not target.resolve().is_relative_to(dest):
        raise FlashError("not a plain file name on this host")
    if entry.type == LFSStat.TYPE_DIR:
        target.mkdir(parents=True, exist_ok=True)
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    with fs.open(path, "rb") as f:
        target.write_bytes(f.read())
    t = mtime(fs, path)
    if t:
        os.utime(target, (t, t))


def cmd_extract(image: bytes, args: argparse.Namespace) -> None:
    fs = mount(image)
    dest = args.dest.resolve()
    files = failed = 0
    # A damaged partition is when this runs, so one unreadable entry must not cost the rest.
    for path, entry in walk(fs):
        try:
            extract_one(fs, path, entry, dest)
        except (FlashError, LittleFSError, OSError, ValueError) as e:
            print(f"error: {host_name(path, 'replace')}: {e}", file=sys.stderr)
            failed += 1
            continue
        if entry.type != LFSStat.TYPE_DIR:
            files += 1
    print(f"{files} files to {args.dest}")
    if failed:
        raise FlashError(f"{failed} entries could not be extracted")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="the board's serial port, e.g. /dev/ttyUSB0")
    source.add_argument(
        "--image", type=Path, help="a dump saved by `dump`, or a whole-flash image"
    )
    parser.add_argument(
        "--baud", type=int, default=921600, help="esptool's baud rate (default 921600)"
    )
    parser.add_argument(
        "--partition",
        help=f"partition label in a whole-flash image or on a board (default {DEFAULT_PARTITION})",
    )
    sub = parser.add_subparsers(dest="command", metavar="command", required=True)

    p = sub.add_parser("dump", help="save the raw partition image")
    p.add_argument("output", type=Path)
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser("ls", help="list every file and directory")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("extract", help="unpack the files into a directory")
    p.add_argument("dest", type=Path)
    p.set_defaults(func=cmd_extract)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        if args.image:
            image = partition_from_image(args.image, args.partition)
        else:
            image = partition_from_board(args.port, args.baud, args.partition)
        args.func(image, args)
    except (FlashError, LittleFSError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
