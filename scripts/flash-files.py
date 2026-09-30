#!/usr/bin/env python3
"""Read and write the LittleFS user partition straight in flash, with the firmware stopped.

The board is reached over its USB serial port with esptool, which holds it in its bootloader
for the command and reboots it afterwards, so this gets to the files where
scripts/device-files.py cannot: a board with no network, or a firmware that does not boot.
`ls` and `extract` read only the blocks the filesystem uses. An image file works too: a dump
saved by `dump`, or a whole-flash image such as QEMU's qemu-flash.bin.

    scripts/flash-files.py --port /dev/ttyUSB0 ls
    scripts/flash-files.py --port /dev/ttyUSB0 extract ./files
    scripts/flash-files.py --port /dev/ttyUSB0 write ./files
    scripts/flash-files.py --port /dev/ttyUSB0 dump littlefs.bin
    scripts/flash-files.py --image littlefs.bin ls
"""

from __future__ import annotations

import argparse
from collections.abc import Callable, Iterator
import contextlib
from dataclasses import dataclass
from datetime import datetime
import hashlib
import io
import os
from pathlib import Path
import posixpath
import struct
import sys

try:
    from littlefs import LFSStat, LittleFS, LittleFSError, UserContext, lfs
except ImportError:
    sys.exit("littlefs-python is missing: pip install -r requirements-dev.txt")

TABLE_OFFSET = 0x8000
TABLE_SIZE = 0xC00
ENTRY = struct.Struct("<2sBBII16sI")
ENTRY_MAGIC = b"\xaa\x50"
MD5_MAGIC = b"\xeb\xeb"
ERASED_MAGIC = b"\xff\xff"
DATA_TYPE = 0x01
# esp_littlefs mounts a data partition of either subtype.
LITTLEFS_SUBTYPE = 0x83
LITTLEFS_SUBTYPES = {0x82, LITTLEFS_SUBTYPE}
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


class ReadFailed(Exception):
    """A block could not be fetched, so nothing read after it can be trusted."""


@dataclass
class Partition:
    name: str
    offset: int
    size: int
    type: int = DATA_TYPE
    subtype: int = LITTLEFS_SUBTYPE


def parse_partition_table(table: bytes) -> list[Partition]:
    partitions = []
    for pos in range(0, len(table) - ENTRY.size + 1, ENTRY.size):
        magic, type_, subtype, offset, size, label, _ = ENTRY.unpack_from(table, pos)
        if magic == MD5_MAGIC:
            # The bootloader refuses a table whose digest disagrees; so do we.
            if table[pos + 16 : pos + 32] != hashlib.md5(table[:pos]).digest():
                raise FlashError("the partition table fails its MD5 check")
            break
        if magic != ENTRY_MAGIC:
            # Past the first entry, anything but the erased tail is damage, as the
            # bootloader sees it.
            if partitions and magic != ERASED_MAGIC:
                raise FlashError("the partition table is damaged")
            break
        name = label.rstrip(b"\0").decode(errors="replace")
        partitions.append(Partition(name, offset, size, type_, subtype))
    return partitions


def find_partition(table: bytes, name: str | None) -> Partition:
    name = name or DEFAULT_PARTITION
    partitions = parse_partition_table(table)
    if not partitions:
        raise FlashError(f"no partition table at {TABLE_OFFSET:#x}")
    for partition in partitions:
        if partition.name == name:
            return partition
    names = ", ".join(p.name for p in partitions)
    raise FlashError(f"no partition named {name!r}; the table has {names}")


def is_littlefs(head: bytes) -> bool:
    """The superblock, in either block of the pair, carries the magic at offset 8."""
    return any(
        head[block + 8 : block + 16] == LITTLEFS_MAGIC for block in (0, BLOCK_SIZE)
    )


def locate_in_image(path: Path, data: bytes, name: str | None) -> Partition:
    """The partition in an image file: all of a dump, or its slice of a whole flash."""
    if is_littlefs(data):
        if name is not None:
            raise FlashError(
                f"{path} is a partition dump: it has no table to pick {name!r} from"
            )
        return Partition(DEFAULT_PARTITION, 0, len(data))
    table = data[TABLE_OFFSET : TABLE_OFFSET + TABLE_SIZE]
    try:
        partition = find_partition(table, name)
    except FlashError as e:
        raise FlashError(f"{path}: not a LittleFS image, and {e}") from None
    end = partition.offset + partition.size
    if end > len(data):
        raise FlashError(
            f"{path}: {partition.name!r} ends at {end:#x}, past the image's end"
        )
    return partition


def partition_from_image(path: Path, name: str | None) -> bytes:
    data = path.read_bytes()
    partition = locate_in_image(path, data, name)
    return data[partition.offset : partition.offset + partition.size]


class Board:
    """One esptool connection for the whole command, ending in a reset into the firmware
    whatever happened, so nothing that fails leaves the board sitting in its bootloader."""

    def __init__(self, port: str, baud: int) -> None:
        # Imported here: only the board needs esptool, and it comes with ESPHome.
        from esptool import cmds
        from esptool.util import FatalError

        self.cmds, self.fatal = cmds, FatalError
        self.esp = self.call(cmds.detect_chip, port)
        try:
            self.esp = self.call(cmds.run_stub, self.esp)
            if baud > self.esp.ESP_ROM_BAUD:
                self.call(self.esp.change_baud, baud)
            self.call(cmds.attach_flash, self.esp)
        except BaseException:
            # The failure that got us here says more than a reset failing after it.
            with contextlib.suppress(Exception):
                self.close()
            raise

    def call(self, fn: Callable, *args, quiet: bool = False, **kwargs):
        # stdout carries the listing; esptool's messages and progress go to stderr, or
        # nowhere for the reads of single blocks, which would bury it in two lines each.
        try:
            with contextlib.redirect_stdout(io.StringIO() if quiet else sys.stderr):
                return fn(*args, **kwargs)
        except self.fatal as e:
            raise FlashError(f"esptool: {e}") from None

    def read(self, offset: int, size: int, progress: bool = False) -> bytes:
        return self.call(
            self.cmds.read_flash,
            self.esp,
            offset,
            size,
            None,
            no_progress=not progress,
            quiet=not progress,
        )

    def write(self, offset: int, data: bytes) -> None:
        self.call(self.cmds.write_flash, self.esp, [(offset, data)])

    def close(self) -> None:
        try:
            self.call(self.cmds.reset_chip, self.esp, "hard-reset")
        finally:
            self.esp._port.close()

    def __enter__(self) -> Board:
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        try:
            self.close()
        except Exception:
            if exc_type is None:
                raise


class Blocks(UserContext):
    """The partition as LittleFS reads it, each block fetched once, when first asked for:
    off a board that means reading only what the filesystem uses."""

    def __init__(self, fetch: Callable[[int], bytes], size: int) -> None:
        self.fetch, self.size, self.cache = fetch, size, {}
        self.failure: BaseException | None = None

    def block(self, n: int) -> bytes:
        if n not in self.cache:
            self.cache[n] = self.fetch(n)
        return self.cache[n]

    def read(self, cfg, block: int, off: int, size: int) -> bytearray:
        # littlefs-python swallows an exception raised in here and carries on with
        # whatever is in the buffer, so the failure is kept for check() to raise.
        if self.failure is None:
            try:
                return bytearray(
                    self.block(block)[off : off + size].ljust(size, b"\xff")
                )
            except BaseException as e:
                self.failure = e
        return bytearray(size)

    def check(self) -> None:
        if self.failure is None:
            return
        if isinstance(self.failure, KeyboardInterrupt):
            raise self.failure
        raise ReadFailed(f"reading the partition: {self.failure}") from self.failure

    @contextlib.contextmanager
    def checked(self) -> Iterator[None]:
        """Around a LittleFS call: a failed fetch is raised in place of whatever LittleFS
        made of the zeros it got instead, an error of its own included."""
        try:
            yield
        finally:
            self.check()


def image_blocks(image: bytes) -> Blocks:
    return Blocks(lambda n: image[n * BLOCK_SIZE : (n + 1) * BLOCK_SIZE], len(image))


@contextlib.contextmanager
def partition_blocks(args: argparse.Namespace) -> Iterator[Blocks]:
    if args.image:
        yield image_blocks(partition_from_image(args.image, args.partition))
        return
    with Board(args.port, args.baud) as board:
        p = find_partition(board.read(TABLE_OFFSET, TABLE_SIZE), args.partition)
        yield Blocks(
            lambda n: board.read(p.offset + n * BLOCK_SIZE, BLOCK_SIZE), p.size
        )


def mount(blocks: Blocks, where: str = "on the partition") -> LittleFS:
    fs = LittleFS(
        context=blocks,
        block_size=BLOCK_SIZE,
        block_count=0,  # from the superblock
        filename_encoding=NAME_ENCODING,
        mount=False,
    )
    try:
        with blocks.checked():
            fs.mount()
    except LittleFSError as e:
        if is_littlefs(blocks.block(0) + blocks.block(1)):
            raise FlashError(
                f"the LittleFS {where} is damaged or cut short: {e}"
            ) from None
        raise FlashError(f"no LittleFS {where}: {e}") from None
    size = fs.block_count * BLOCK_SIZE
    if size > blocks.size:
        raise FlashError(
            f"the LittleFS {where} is truncated: "
            f"{blocks.size} bytes of a {size}-byte filesystem"
        )
    return fs


def host_name(path: str, errors: str) -> str:
    return path.encode(NAME_ENCODING).decode("utf-8", errors)


def walk(fs: LittleFS, top: str = "/") -> Iterator[tuple[str, LFSStat]]:
    """Every entry under top, sorted by name, each directory followed by what it holds."""
    with fs.context.checked():
        entries = sorted(fs.scandir(top), key=lambda e: e.name)
    for entry in entries:
        path = posixpath.join(top, entry.name)
        yield path, entry
        if entry.type == LFSStat.TYPE_DIR:
            yield from walk(fs, path)


def mtime(fs: LittleFS, path: str) -> int | None:
    with fs.context.checked():
        try:
            raw = fs.getattr(path, MTIME_ATTR)
        except LittleFSError:
            raw = b""
    t = int.from_bytes(raw, "little", signed=True)
    try:
        # A value no date fits comes from a damaged attribute; the file is still worth reading.
        datetime.fromtimestamp(t)
    except (ValueError, OverflowError, OSError):
        return None
    return t or None


def cmd_dump(args: argparse.Namespace) -> None:
    # Every byte, not just the used blocks: a dump is also the copy of a partition too
    # damaged to walk.
    if args.image:
        image = partition_from_image(args.image, args.partition)
    else:
        with Board(args.port, args.baud) as board:
            p = find_partition(board.read(TABLE_OFFSET, TABLE_SIZE), args.partition)
            image = board.read(p.offset, p.size, progress=True)
    args.output.write_bytes(image)
    print(f"{len(image)} bytes to {args.output}")


def cmd_ls(args: argparse.Namespace) -> None:
    with partition_blocks(args) as blocks:
        fs = mount(blocks)
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
    # On Windows, \ and : split a name into folders, a drive or an NTFS stream.
    splits = WINDOWS and any(c in name for c in "\\:")
    if splits or not target.resolve().is_relative_to(dest):
        raise FlashError("not a plain file name on this host")
    if entry.type == LFSStat.TYPE_DIR:
        target.mkdir(parents=True, exist_ok=True)
        return
    target.parent.mkdir(parents=True, exist_ok=True)
    with fs.context.checked(), fs.open(path, "rb") as f:
        data = f.read()
    target.write_bytes(data)
    t = mtime(fs, path)
    if t:
        os.utime(target, (t, t))


def cmd_extract(args: argparse.Namespace) -> None:
    dest = args.dest.resolve()
    files = failed = 0
    with partition_blocks(args) as blocks:
        fs = mount(blocks)
        # A damaged partition is when this runs, so one unreadable entry must not cost
        # the rest.
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


def pack(source: Path, size: int) -> bytes:
    """A LittleFS image of the directory, with the geometry esp_littlefs mounts."""
    fs = LittleFS(
        block_size=BLOCK_SIZE,
        block_count=size // BLOCK_SIZE,
        filename_encoding=NAME_ENCODING,
    )
    for path in sorted(source.rglob("*")):
        parts = path.relative_to(source).parts
        name = "/" + "/".join(os.fsencode(p).decode(NAME_ENCODING) for p in parts)
        try:
            if path.is_symlink() and path.is_dir():
                # rglob does not follow it, so it would arrive as an empty directory.
                raise FlashError(f"{path}: a link to a directory is not packed")
            if path.is_dir():
                fs.mkdir(name)
                continue
            data = path.read_bytes()
            # FileHandle would close a handle littlefs already let go of on NOSPC
            # a second time, which aborts the process.
            fh = lfs.file_open(fs.fs, name, "wb", NAME_ENCODING)
            try:
                lfs.file_write(fs.fs, fh, data)
            finally:
                lfs.file_close(fs.fs, fh)
            t = int(path.stat().st_mtime)
            fs.setattr(name, MTIME_ATTR, t.to_bytes(8, "little", signed=True))
        except LittleFSError as e:
            if e.code == LittleFSError.Error.LFS_ERR_NOSPC:
                raise FlashError(f"{source} does not fit in {size} bytes") from None
            raise FlashError(f"{path}: {e}") from None
    return bytes(fs.context.buffer)


def build_image(source: Path, size: int) -> bytes:
    """The partition's new contents: a directory packed, or a saved partition image."""
    image = pack(source, size) if source.is_dir() else source.read_bytes()
    # The firmware formats a partition it cannot mount, so nothing goes out that would not.
    fs = mount(image_blocks(image), where=f"in {source}")
    fs_size = fs.block_count * BLOCK_SIZE
    if len(image) != size or fs_size != size:
        raise FlashError(
            f"{source}: a {fs_size}-byte filesystem in {len(image)} bytes, "
            f"for a {size}-byte partition"
        )
    return image


def check_writable(partition: Partition) -> None:
    if partition.type != DATA_TYPE or partition.subtype not in LITTLEFS_SUBTYPES:
        raise FlashError(
            f"{partition.name!r} is not a partition LittleFS lives in "
            f"(type {partition.type:#04x}, subtype {partition.subtype:#04x})"
        )


def cmd_write(args: argparse.Namespace) -> None:
    if args.image:
        data = args.image.read_bytes()
        partition = locate_in_image(args.image, data, args.partition)
        check_writable(partition)
        image = build_image(args.source, partition.size)
        with args.image.open("r+b") as f:
            f.seek(partition.offset)
            f.write(image)
    else:
        with Board(args.port, args.baud) as board:
            partition = find_partition(
                board.read(TABLE_OFFSET, TABLE_SIZE), args.partition
            )
            check_writable(partition)
            image = build_image(args.source, partition.size)
            board.write(partition.offset, image)
    print(f"{args.source} to {partition.name!r} at {partition.offset:#x}")


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

    p = sub.add_parser("dump", help="save the raw partition image, every byte of it")
    p.add_argument("output", type=Path)
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser("ls", help="list every file and directory")
    p.set_defaults(func=cmd_ls)

    p = sub.add_parser("extract", help="unpack the files into a directory")
    p.add_argument("dest", type=Path)
    p.set_defaults(func=cmd_extract)

    p = sub.add_parser(
        "write", help="replace the whole partition with a directory or a saved dump"
    )
    p.add_argument("source", type=Path, help="a directory, or an image `dump` saved")
    p.set_defaults(func=cmd_write)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        args.func(args)
    except (FlashError, ReadFailed, LittleFSError, OSError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    return 0


if __name__ == "__main__":
    sys.exit(main())
