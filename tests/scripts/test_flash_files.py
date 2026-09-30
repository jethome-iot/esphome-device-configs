"""scripts/flash-files.py: finding the user partition in an image or on a board, reading and writing it."""

import contextlib
import errno
import functools
import hashlib
import importlib.util
import io
import os
import random
import re
import struct
import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from unittest import mock

from esptool import cmds as esptool_cmds
from esptool.util import FatalError
from littlefs import LittleFS

SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "flash-files.py"
_spec = importlib.util.spec_from_file_location("flash_files", SCRIPT)
flash_files = importlib.util.module_from_spec(_spec)
# @dataclass looks its module up in sys.modules.
sys.modules[_spec.name] = flash_files
_spec.loader.exec_module(flash_files)

BLOCK_SIZE = 4096
LFS_BLOCKS = 16
LFS_OFFSET = 0x20000
LFS_SIZE = LFS_BLOCKS * BLOCK_SIZE
FLASH_SIZE = LFS_OFFSET + LFS_SIZE

DIRS = ["/config", "/config/backup", "/crash", "/empty"]
FILES = {
    "/automations.json": b'{"rules": []}',
    "/config/backup/wifi.json": b'{"ssid": "old"}',
    "/config/mqtt.json": b'{"broker": "10.0.0.2"}',
    "/config/wifi.json": b'{"ssid": "jethome"}',
    # Past one block, so it is not inlined into its directory; every byte value, NUL included.
    "/crash/panic.bin": random.Random(1).randbytes(6000),
    "/version.txt": b"1.2.3\n",
}
TREE = {**dict.fromkeys(DIRS), **FILES}
# The one file with blocks of its own: the rest live in their directory's metadata.
BIG_FILE = "/crash/panic.bin"
STAMPED = "/config/wifi.json"
MTIME = 1_700_000_000

ENTRY = struct.Struct("<2sBBII16sI")
APP, DATA = 0x00, 0x01
PARTITIONS = [
    ("nvs", DATA, 0x02, 0x9000, 0x5000),
    ("otadata", DATA, 0x00, 0xE000, 0x2000),
    ("app0", APP, 0x10, 0x10000, 0x10000),
    ("littlefs", DATA, 0x83, LFS_OFFSET, LFS_SIZE),
]
NVS = slice(0x9000, 0xE000)


def littlefs_image() -> bytes:
    fs = LittleFS(block_size=BLOCK_SIZE, block_count=LFS_BLOCKS)
    for path in DIRS:
        fs.mkdir(path)
    for path, data in FILES.items():
        with fs.open(path, "wb") as f:
            f.write(data)
    fs.setattr(STAMPED, "t", MTIME.to_bytes(8, "little", signed=True))
    return bytes(fs.context.buffer)


def partition_table(partitions, md5: bool = True, after_md5=()) -> bytes:
    """As gen_esp32part writes it: the entries, the MD5 entry over them, then erased flash."""

    def entries(rows):
        return b"".join(
            ENTRY.pack(b"\xaa\x50", type_, subtype, offset, size, name.encode(), 0)
            for name, type_, subtype, offset, size in rows
        )

    table = entries(partitions)
    if md5:
        table += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(table).digest()
    table += entries(after_md5)
    return table.ljust(flash_files.TABLE_SIZE, b"\xff")


def flash_image(littlefs: bytes, partitions=PARTITIONS) -> bytes:
    flash = bytearray(b"\xff" * FLASH_SIZE)
    flash[0x8000 : 0x8000 + flash_files.TABLE_SIZE] = partition_table(partitions)
    flash[NVS] = b"nvs!" * ((NVS.stop - NVS.start) // 4)
    flash[LFS_OFFSET : LFS_OFFSET + LFS_SIZE] = littlefs
    return bytes(flash)


LFS = littlefs_image()
FLASH = flash_image(LFS)
ERASED_FLASH = flash_image(b"\xff" * LFS_SIZE)


def run_main(*argv) -> tuple[int, str, str]:
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = flash_files.main([str(a) for a in argv])
    return code, out.getvalue(), err.getvalue()


class TempDirTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.tmp = Path(tmp.name)

    def write(self, name: str, data: bytes) -> Path:
        path = self.tmp / name
        path.write_bytes(data)
        return path

    def run_ok(self, *argv) -> str:
        code, out, err = run_main(*argv)
        self.assertEqual((code, err), (0, ""))
        return out

    def assertFails(self, argv, message: str):
        code, out, err = run_main(*argv)
        self.assertEqual(code, 1)
        self.assertEqual(out, "")
        self.assertTrue(err.startswith("error: "), err)
        self.assertIn(message, err)


class PartitionTable(unittest.TestCase):
    def test_the_entries_up_to_the_md5_entry_are_read(self):
        # An entry after the MD5 one is not part of the table.
        stray = [("stray", DATA, 0x82, 0x40000, 0x1000)]
        parsed = flash_files.parse_partition_table(
            partition_table(PARTITIONS, after_md5=stray)
        )
        self.assertEqual(
            [(p.name, p.type, p.subtype, p.offset, p.size) for p in parsed], PARTITIONS
        )

    def test_a_table_without_md5_ends_at_the_erased_tail(self):
        parsed = flash_files.parse_partition_table(
            partition_table(PARTITIONS, md5=False)
        )
        self.assertEqual([p.name for p in parsed], [p[0] for p in PARTITIONS])

    def test_a_table_whose_md5_disagrees_is_refused(self):
        # A flipped bit in littlefs's offset: without the check it would read the wrong range.
        table = bytearray(partition_table(PARTITIONS))
        table[3 * ENTRY.size + 4] ^= 0x01
        with self.assertRaisesRegex(
            flash_files.FlashError, "the partition table fails its MD5 check"
        ):
            flash_files.find_partition(bytes(table), "littlefs")

    def test_a_damaged_entry_before_the_md5_is_refused(self):
        # Ending the table there would skip the digest and trust the entries before it.
        table = bytearray(partition_table(PARTITIONS))
        table[2 * ENTRY.size] ^= 0x01
        with self.assertRaisesRegex(
            flash_files.FlashError, "the partition table is damaged"
        ):
            flash_files.find_partition(bytes(table), "nvs")

    def test_a_partition_is_found_by_name(self):
        partition = flash_files.find_partition(partition_table(PARTITIONS), "nvs")
        self.assertEqual(
            partition, flash_files.Partition("nvs", 0x9000, 0x5000, DATA, 0x02)
        )

    def test_a_missing_name_is_refused_with_the_names_the_table_has(self):
        with self.assertRaisesRegex(
            flash_files.FlashError,
            "no partition named 'spiffs'; the table has nvs, otadata, app0, littlefs$",
        ):
            flash_files.find_partition(partition_table(PARTITIONS), "spiffs")

    def test_an_empty_table_is_refused(self):
        for table in (b"\xff" * flash_files.TABLE_SIZE, b""):
            with (
                self.subTest(size=len(table)),
                self.assertRaisesRegex(
                    flash_files.FlashError, "no partition table at 0x8000"
                ),
            ):
                flash_files.find_partition(table, "littlefs")


class Writable(unittest.TestCase):
    def test_only_a_data_partition_of_a_littlefs_subtype_is_written(self):
        # esp_littlefs mounts either data subtype; nvs and the app are not its to hold.
        for type_, subtype, writable in (
            (DATA, 0x83, True),
            (DATA, 0x82, True),
            (DATA, 0x02, False),
            # The number means something else under another type.
            (APP, 0x83, False),
        ):
            partition = flash_files.Partition("p", LFS_OFFSET, LFS_SIZE, type_, subtype)
            with self.subTest(type=type_, subtype=subtype):
                if writable:
                    flash_files.check_writable(partition)
                    continue
                with self.assertRaisesRegex(
                    flash_files.FlashError,
                    f"^'p' is not a partition LittleFS lives in "
                    f"\\(type {type_:#04x}, subtype {subtype:#04x}\\)$",
                ):
                    flash_files.check_writable(partition)

    def test_an_encrypted_partition_is_not_written(self):
        # esptool refuses plain data there only without force, which the write passes.
        partition = flash_files.Partition("p", LFS_OFFSET, LFS_SIZE, encrypted=True)
        with self.assertRaisesRegex(flash_files.FlashError, "^'p' is encrypted"):
            flash_files.check_writable(partition)

    def test_a_partition_off_a_sector_or_over_the_table_is_not_written(self):
        # Erasing it would take a neighbour's sector, or the table itself, along.
        for offset in (LFS_OFFSET + 0x100, 0x8000, 0x0):
            partition = flash_files.Partition("p", offset, LFS_SIZE)
            with (
                self.subTest(offset=hex(offset)),
                self.assertRaisesRegex(
                    flash_files.FlashError,
                    f"^'p' at {offset:#x} does not start on a sector past the partition table$",
                ),
            ):
                flash_files.check_writable(partition)
        flash_files.check_writable(flash_files.Partition("p", 0x9000, LFS_SIZE))

    def test_the_encrypted_flag_is_read_from_the_table(self):
        table = bytearray(partition_table(PARTITIONS))
        # The flags word closes each 32-byte entry; bit 0 is "encrypted".
        table[3 * ENTRY.size + 28] |= 0x01
        table[4 * ENTRY.size + 16 :] = b"\xff" * (len(table) - 4 * ENTRY.size - 16)
        md5 = hashlib.md5(bytes(table[: 4 * ENTRY.size])).digest()
        table[4 * ENTRY.size + 16 : 4 * ENTRY.size + 32] = md5
        parsed = flash_files.parse_partition_table(bytes(table))
        self.assertEqual([p.encrypted for p in parsed], [False, False, False, True])


class ImageSource(TempDirTestCase):
    def test_a_raw_dump_is_returned_as_is(self):
        path = self.write("littlefs.bin", LFS)
        self.assertEqual(flash_files.partition_from_image(path, None), LFS)

    def test_a_raw_dump_is_recognised_by_either_superblock(self):
        # Power lost mid-erase can leave block 0 blank; block 1 of the pair still has it.
        dump = b"\xff" * BLOCK_SIZE + LFS[BLOCK_SIZE:]
        path = self.write("littlefs.bin", dump)
        self.assertEqual(flash_files.partition_from_image(path, None), dump)

    def test_partition_is_refused_for_a_raw_dump(self):
        # It has no table, so a label cannot be honoured; silently dumping LittleFS would lie.
        path = self.write("littlefs.bin", LFS)
        self.assertFails(
            ["--image", path, "--partition", "nvs", "dump", self.tmp / "nvs.bin"],
            f"{path} is a partition dump: it has no table to pick 'nvs' from",
        )
        self.assertFalse((self.tmp / "nvs.bin").exists())

    def test_a_truncated_dump_is_refused(self):
        # All of its metadata sits in the superblock pair, so it still mounts.
        fs = LittleFS(block_size=BLOCK_SIZE, block_count=LFS_BLOCKS)
        with fs.open("/a.txt", "wb") as f:
            f.write(b"a")
        path = self.write("littlefs.bin", bytes(fs.context.buffer)[: 2 * BLOCK_SIZE])
        for command in (["ls"], ["extract", self.tmp / "files"]):
            with self.subTest(command=command[0]):
                self.assertFails(
                    ["--image", path, *command],
                    f"the LittleFS on the partition is truncated: "
                    f"{2 * BLOCK_SIZE} bytes of a {LFS_SIZE}-byte filesystem",
                )

    def test_a_filesystem_that_does_not_mount_is_called_damaged(self):
        # The superblock is there, the directories it points at are not.
        path = self.write("littlefs.bin", LFS[: 3 * BLOCK_SIZE])
        self.assertFails(
            ["--image", path, "ls"],
            "the LittleFS on the partition is damaged or cut short",
        )

    def test_a_whole_flash_image_yields_exactly_the_partition(self):
        path = self.write("flash.bin", FLASH)
        self.assertEqual(flash_files.partition_from_image(path, None), LFS)
        self.assertEqual(flash_files.partition_from_image(path, "littlefs"), LFS)

    def test_partition_picks_another_label(self):
        output = self.tmp / "nvs.bin"
        code, _, err = run_main(
            "--image",
            self.write("flash.bin", FLASH),
            "--partition",
            "nvs",
            "dump",
            output,
        )
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(output.read_bytes(), FLASH[NVS])

    def test_a_partition_past_the_image_end_is_refused(self):
        path = self.write("flash.bin", FLASH[: LFS_OFFSET + 0x8000])
        self.assertFails(
            ["--image", path, "ls"],
            f"{path}: 'littlefs' ends at 0x30000, past the image's end",
        )

    def test_a_missing_label_in_a_whole_flash_image_is_refused(self):
        path = self.write("flash.bin", FLASH)
        self.assertFails(
            ["--image", path, "--partition", "spiffs", "ls"],
            f"{path}: not a LittleFS image, and no partition named 'spiffs'",
        )

    def test_a_file_that_is_neither_is_refused(self):
        for name, data in (
            ("short.bin", b"hello"),
            ("erased.bin", b"\xff" * FLASH_SIZE),
            ("zeros.bin", bytes(FLASH_SIZE)),
        ):
            with self.subTest(name=name):
                path = self.write(name, data)
                self.assertFails(
                    ["--image", path, "ls"],
                    f"{path}: not a LittleFS image, and no partition table at 0x8000",
                )

    def test_a_missing_file_is_an_error(self):
        self.assertFails(["--image", self.tmp / "absent.bin", "ls"], "absent.bin")


LS_LINE = re.compile(r"^ *(\d*)  (.{19})  (/\S*)$")


class Ls(TempDirTestCase):
    def setUp(self):
        super().setUp()
        code, out, err = run_main("--image", self.write("flash.bin", FLASH), "ls")
        self.assertEqual((code, err), (0, ""))
        self.lines = out.splitlines()
        self.rows = {}
        for line in self.lines:
            match = LS_LINE.match(line)
            self.assertIsNotNone(match, line)
            size, when, path = match.groups()
            self.rows[path] = (size, when.strip())

    def test_the_tree_is_listed_depth_first_in_name_order(self):
        # Each directory right before its contents; files and directories sorted together.
        self.assertEqual(
            [line.split()[-1] for line in self.lines],
            [
                "/automations.json",
                "/config/",
                "/config/backup/",
                "/config/backup/wifi.json",
                "/config/mqtt.json",
                "/config/wifi.json",
                "/crash/",
                "/crash/panic.bin",
                "/empty/",
                "/version.txt",
            ],
        )

    def test_directories_have_a_trailing_slash_and_no_size(self):
        for path in DIRS:
            self.assertEqual(self.rows[path + "/"], ("", ""))

    def test_files_show_their_size(self):
        for path, data in FILES.items():
            self.assertEqual(self.rows[path][0], str(len(data)), path)

    def test_the_mtime_is_shown_only_where_the_file_has_one(self):
        # Local time, like the host's other tools.
        when = datetime.fromtimestamp(MTIME).strftime("%Y-%m-%d %H:%M:%S")
        for path in FILES:
            self.assertEqual(self.rows[path][1], when if path == STAMPED else "", path)


class Extract(TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.dest = self.tmp / "files"
        code, self.out, err = run_main(
            "--image", self.write("littlefs.bin", LFS), "extract", self.dest
        )
        self.assertEqual((code, err), (0, ""))

    def test_files_are_byte_identical(self):
        for path, data in FILES.items():
            with self.subTest(path=path):
                self.assertEqual((self.dest / path.lstrip("/")).read_bytes(), data)

    def test_an_empty_directory_is_created(self):
        self.assertTrue((self.dest / "empty").is_dir())
        self.assertEqual(list((self.dest / "empty").iterdir()), [])

    def test_nothing_else_is_written(self):
        written = {
            "/" + p.relative_to(self.dest).as_posix() for p in self.dest.rglob("*")
        }
        self.assertEqual(written, set(DIRS) | set(FILES))

    def test_the_mtime_is_applied_where_the_file_has_one(self):
        self.assertEqual(os.stat(self.dest / STAMPED.lstrip("/")).st_mtime, MTIME)
        self.assertNotEqual(os.stat(self.dest / "config/mqtt.json").st_mtime, MTIME)

    def test_the_summary_counts_files(self):
        self.assertEqual(self.out, f"{len(FILES)} files to {self.dest}\n")


class DamagedMtime(TempDirTestCase):
    """A file whose mtime no date fits is listed and extracted as if it had none."""

    def setUp(self):
        super().setUp()
        fs = LittleFS(block_size=BLOCK_SIZE, block_count=LFS_BLOCKS)
        for name, t in [("past-os", 2**62), ("past-9999", 253_402_300_800)]:
            with fs.open(f"/{name}.txt", "wb") as f:
                f.write(name.encode())
            fs.setattr(f"/{name}.txt", "t", t.to_bytes(8, "little", signed=True))
        self.image = self.write("littlefs.bin", bytes(fs.context.buffer))

    def test_ls_leaves_the_time_blank(self):
        code, out, err = run_main("--image", self.image, "ls")
        self.assertEqual((code, err), (0, ""))
        for line in out.splitlines():
            self.assertEqual(LS_LINE.match(line).group(2).strip(), "", line)

    def test_extract_keeps_the_files(self):
        dest = self.tmp / "files"
        code, out, err = run_main("--image", self.image, "extract", dest)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual((dest / "past-os.txt").read_bytes(), b"past-os")
        self.assertEqual((dest / "past-9999.txt").read_bytes(), b"past-9999")


class ExtractPastFailures(TempDirTestCase):
    """A damaged partition is what extract is for, so one bad entry must not cost the rest."""

    def test_the_other_files_are_still_extracted(self):
        dest = self.tmp / "files"
        # A directory where a file belongs: that one write fails, the rest must not.
        (dest / "version.txt").mkdir(parents=True)
        code, out, err = run_main(
            "--image", self.write("littlefs.bin", LFS), "extract", dest
        )
        self.assertEqual(code, 1)
        self.assertEqual(out, f"{len(FILES) - 1} files to {dest}\n")
        self.assertTrue(err.startswith("error: /version.txt: "), err)
        self.assertIn("error: 1 entries could not be extracted", err)
        for path, data in FILES.items():
            if path != "/version.txt":
                self.assertEqual((dest / path.lstrip("/")).read_bytes(), data, path)


class FileNames(TempDirTestCase):
    """LittleFS names are bytes; the firmware stores whatever a client sent."""

    UTF8 = "/привет.txt".encode()
    LATIN1 = b"/caf\xe9.txt"

    def setUp(self):
        super().setUp()
        # latin-1 hands the raw bytes to LittleFS unchanged.
        fs = LittleFS(
            block_size=BLOCK_SIZE, block_count=LFS_BLOCKS, filename_encoding="latin-1"
        )
        for name in (self.UTF8, self.LATIN1):
            with fs.open(name.decode("latin-1"), "wb") as f:
                f.write(name)
        self.image = self.write("littlefs.bin", bytes(fs.context.buffer))

    def test_ls_shows_utf8_names_and_marks_the_rest(self):
        code, out, err = run_main("--image", self.image, "ls")
        self.assertEqual((code, err), (0, ""))
        paths = [LS_LINE.match(line).group(3) for line in out.splitlines()]
        self.assertEqual(sorted(paths), ["/caf�.txt", "/привет.txt"])

    def test_a_name_windows_would_split_is_refused_there(self):
        # There "report.txt:payload" is an NTFS stream of report.txt, and "a\\b" is a folder.
        refused = ["/report.txt:payload", "/a\\b"]
        fs = LittleFS(block_size=BLOCK_SIZE, block_count=LFS_BLOCKS)
        for name in [*refused, "/report.txt"]:
            with fs.open(name, "wb") as f:
                f.write(name.encode())
        image = self.write("names.bin", bytes(fs.context.buffer))
        dest = self.tmp / "files"
        with mock.patch.object(flash_files, "WINDOWS", True):
            code, out, err = run_main("--image", image, "extract", dest)
        self.assertEqual(code, 1)
        self.assertEqual(out, f"1 files to {dest}\n")
        for name in refused:
            self.assertIn(f"error: {name}: not a plain file name on this host", err)
        self.assertEqual(os.listdir(dest), ["report.txt"])

    def test_a_name_that_is_not_utf8_is_refused_on_windows(self):
        # Windows would store it as other bytes, and a write back would carry those.
        dest = self.tmp / "files"
        with mock.patch.object(flash_files, "WINDOWS", True):
            code, out, err = run_main("--image", self.image, "extract", dest)
        self.assertEqual((code, out), (1, f"1 files to {dest}\n"))
        self.assertIn(
            "error: /caf\ufffd.txt: not UTF-8, which this host cannot name a file", err
        )
        self.assertEqual(os.listdir(dest), ["привет.txt"])

    def test_two_entries_the_host_takes_for_one_file_are_not_merged(self):
        # A case-folding host makes /a.txt and /A.txt one file; a link in DEST stands in.
        fs = LittleFS(block_size=BLOCK_SIZE, block_count=LFS_BLOCKS)
        for name in ("/a.txt", "/b.txt"):
            with fs.open(name, "wb") as f:
                f.write(name.encode())
        image = self.write("pair.bin", bytes(fs.context.buffer))
        dest = self.tmp / "files"
        dest.mkdir()
        try:
            (dest / "b.txt").symlink_to("a.txt")
        except (OSError, NotImplementedError):
            self.skipTest("no symlinks here")
        code, out, err = run_main("--image", image, "extract", dest)
        self.assertEqual((code, out), (1, f"1 files to {dest}\n"))
        self.assertIn(
            "error: /b.txt: on this host that is the file another entry was written to",
            err,
        )
        self.assertEqual((dest / "a.txt").read_bytes(), b"/a.txt")

    def test_extract_keeps_the_bytes_of_every_name(self):
        dest = self.tmp / "files"
        code, _, err = run_main("--image", self.image, "extract", dest)
        self.assertEqual((code, err), (0, ""))
        for name in (self.UTF8, self.LATIN1):
            with open(os.fsencode(dest) + name, "rb") as f:
                self.assertEqual(f.read(), name)


class Dump(TempDirTestCase):
    def test_dump_writes_the_partition(self):
        output = self.tmp / "littlefs.bin"
        code, out, _ = run_main(
            "--image", self.write("flash.bin", FLASH), "dump", output
        )
        self.assertEqual(code, 0)
        self.assertEqual(output.read_bytes(), LFS)
        self.assertEqual(out, f"{LFS_SIZE} bytes to {output}\n")

    def test_an_unwritable_output_is_an_error(self):
        image = self.write("flash.bin", FLASH)
        output = self.tmp / "no" / "such" / "out.bin"
        self.assertFails(["--image", image, "dump", output], "out.bin")


class LostBlock(unittest.TestCase):
    def test_mtime_does_not_take_a_lost_block_for_a_file_without_one(self):
        blocks = flash_files.image_blocks(LFS)
        fs = flash_files.mount(blocks)

        def lost(n):
            raise OSError(errno.EIO, "Input/output error")

        # Mount fetches every metadata block; forgetting them sends the next read out.
        blocks.cache.clear()
        blocks.fetch = lost
        with self.assertRaisesRegex(
            flash_files.ReadFailed, r"^reading the partition: \[Errno 5\]"
        ):
            flash_files.mtime(fs, STAMPED)

    def test_a_lost_block_is_reported_over_what_littlefs_makes_of_it(self):
        # LittleFS takes the zeros it got for a directory or a skip-list pointer as
        # corruption and says so first; the lost read is the reason to report.
        for error, raised in (
            (OSError(errno.EIO, "Input/output error"), flash_files.ReadFailed),
            (KeyboardInterrupt(), KeyboardInterrupt),
        ):
            with self.subTest(error=type(error).__name__):
                blocks = flash_files.image_blocks(LFS)
                fs = flash_files.mount(blocks)
                entries = dict(flash_files.walk(fs))

                def lost(n, error=error):
                    raise error

                blocks.cache.clear()
                blocks.fetch = lost
                with self.assertRaises(raised):
                    list(flash_files.walk(fs))
                blocks.failure = None
                with tempfile.TemporaryDirectory() as dest, self.assertRaises(raised):
                    flash_files.extract_one(
                        fs, "/crash/panic.bin", entries["/crash/panic.bin"], Path(dest)
                    )


class FakeChip:
    """A loader, the ROM's or the stub's: the ROM loader's rate, and the serial port."""

    ESP_ROM_BAUD = 115200

    def __init__(self, esptool: "FakeEsptool", port=None, stub: bool = False):
        self.esptool, self.stub = esptool, stub
        self._port = port or mock.Mock(close=lambda: esptool.ops.append(("close",)))

    def change_baud(self, baud: int) -> None:
        self.esptool.op("baud", baud, esp=self)


class FakeEsptool:
    """Stands in for esptool's Python API: every call logged in ops, the flash a bytearray.

    The call named by fail raises error from its at-th time on, as a lost link stays lost.
    """

    # Starts every line it prints, to tell them from the script's own.
    SAYS = "[esptool] "

    def __init__(
        self,
        flash: bytes,
        fail: str = "",
        at: int = 1,
        error: BaseException | None = None,
    ):
        self.flash = bytearray(flash)
        self.fail, self.at = fail, at
        self.error = error or FatalError("Timed out waiting for packet header")
        self.ops = []
        self.written = []
        self.write_options = []
        self.connect_baud = None

    def op(self, name: str, *args, esp: FakeChip | None = None) -> None:
        # The ROM loader reads at a crawl and changes the rate another way.
        assert esp is None or esp.stub, f"{name} through the ROM loader"
        self.ops.append((name, *args))
        # Printed as esptool does, to whatever sys.stdout is at the time.
        print(f"{self.SAYS}{name}")
        done = sum(op[0] == name for op in self.ops)
        if name == self.fail and done >= self.at:
            # A fresh traceback each time: one error serves a run of subtests.
            raise self.error.with_traceback(None)

    def detect_chip(self, port: str, baud: int = 115200, *args, **kwargs) -> FakeChip:
        self.connect_baud = baud
        self.op("detect", port)
        return FakeChip(self)

    def run_stub(self, esp: FakeChip, *args, **kwargs) -> FakeChip:
        self.op("stub")
        return FakeChip(self, esp._port, stub=True)

    def attach_flash(self, esp: FakeChip, *args, **kwargs) -> None:
        self.op("attach", esp=esp)

    def read_flash(
        self, esp, address, size, output=None, flash_size="keep", no_progress=False
    ) -> bytes:
        self.op("read", address, size, not no_progress, esp=esp)
        return bytes(self.flash[address : address + size])

    def write_flash(self, esp, addr_data, **kwargs) -> None:
        self.write_options.append(kwargs)
        for address, data in addr_data:
            self.op("write", address, len(data), esp=esp)
            self.written.append((address, bytes(data)))
            self.flash[address : address + len(data)] = data

    def reset_chip(self, esp, reset_mode: str = "hard-reset") -> None:
        self.op("reset", reset_mode)

    @contextlib.contextmanager
    def installed(self):
        # Specced on the real functions: a call esptool's signatures no longer take fails.
        fakes = {
            name: mock.create_autospec(
                getattr(esptool_cmds, name), side_effect=getattr(self, name)
            )
            for name in (
                "detect_chip",
                "run_stub",
                "attach_flash",
                "read_flash",
                "write_flash",
                "reset_chip",
            )
        }
        with mock.patch.multiple(esptool_cmds, **fakes):
            yield

    def blocks_read(self) -> list[int]:
        """The partition's blocks read one by one, in order; the table read aside."""
        return [
            (op[1] - LFS_OFFSET) // BLOCK_SIZE
            for op in self.ops
            if op[0] == "read" and op[1] != flash_files.TABLE_OFFSET
        ]


def script_lines(err: str) -> str:
    """stderr without what esptool printed."""
    return "".join(
        line
        for line in err.splitlines(keepends=True)
        if not line.startswith(FakeEsptool.SAYS)
    )


PORT = "/dev/ttyUSB7"
# The script's default rate, past the ROM loader's.
BAUD = 921600
OPEN = [("detect", PORT), ("stub",), ("baud", BAUD), ("attach",)]
TABLE_READ = ("read", 0x8000, 0xC00, False)
# Back into the firmware, then the port let go.
CLOSE = [("reset", "hard-reset"), ("close",)]


class BoardTestCase(TempDirTestCase):
    def run_board(self, esptool: FakeEsptool, *argv) -> tuple[int, str, str]:
        with esptool.installed():
            return run_main("--port", PORT, *argv)

    def run_board_ok(self, esptool: FakeEsptool, *argv) -> str:
        code, out, err = self.run_board(esptool, *argv)
        self.assertEqual((code, script_lines(err)), (0, ""))
        return out

    def assertBoardFails(self, esptool: FakeEsptool, argv, message: str, code: int = 1):
        got, out, err = self.run_board(esptool, *argv)
        err = script_lines(err)
        self.assertEqual(got, code, err)
        self.assertEqual(out, "")
        if code == 1:
            self.assertTrue(err.startswith("error: "), err)
            self.assertIn(message, err)
        else:
            self.assertEqual(err, "")
        # Whatever went wrong, the board is left running its firmware.
        self.assertEqual(esptool.ops[-2:], CLOSE)
        self.assertEqual(esptool.ops.count(CLOSE[0]), 1)


class BoardSession(BoardTestCase):
    """One esptool connection per command, ending in a reset whatever happened."""

    def test_the_stub_is_started_and_sped_up_before_the_flash_is_attached(self):
        esptool = FakeEsptool(FLASH)
        self.run_board_ok(esptool, "ls")
        self.assertEqual(esptool.ops[:5], [*OPEN, TABLE_READ])

    def test_the_rate_is_changed_only_above_the_rom_loaders(self):
        for baud in (115200, 9600):
            with self.subTest(baud=baud):
                esptool = FakeEsptool(FLASH)
                self.run_board_ok(esptool, "--baud", baud, "ls")
                self.assertEqual(
                    esptool.ops[:4],
                    [("detect", PORT), ("stub",), ("attach",), TABLE_READ],
                )
                # A rate below the ROM loader's is the one to connect at.
                self.assertEqual(esptool.connect_baud, baud)

    def test_a_faster_rate_is_reached_from_the_rom_loaders(self):
        esptool = FakeEsptool(FLASH)
        self.run_board_ok(esptool, "ls")
        self.assertEqual(
            (esptool.connect_baud, esptool.ops[2]), (115200, ("baud", BAUD))
        )

    def test_every_command_ends_in_one_reset_then_the_port_closed(self):
        source = make_source(self.tmp / "src")
        for command in (
            ["ls"],
            ["extract", self.tmp / "files"],
            ["dump", self.tmp / "littlefs.bin"],
            ["write", source],
        ):
            with self.subTest(command=command[0]):
                esptool = FakeEsptool(FLASH)
                self.run_board_ok(esptool, *command)
                self.assertEqual(esptool.ops[-2:], CLOSE)
                self.assertEqual(esptool.ops.count(CLOSE[0]), 1)

    def test_esptool_is_heard_on_stderr_but_not_over_each_read(self):
        source = make_source(self.tmp / "src")
        connect = ["detect", "stub", "baud", "attach"]
        for command, heard in (
            # The table and every block read in silence: two lines each would bury the rest.
            (["ls"], [*connect, "reset"]),
            (["extract", self.tmp / "files"], [*connect, "reset"]),
            # The one long read, with its progress.
            (["dump", self.tmp / "littlefs.bin"], [*connect, "read", "reset"]),
            (["write", source], [*connect, "write", "reset"]),
        ):
            with self.subTest(command=command[0]):
                esptool = FakeEsptool(FLASH)
                code, out, err = self.run_board(esptool, *command)
                self.assertEqual((code, script_lines(err)), (0, ""))
                self.assertNotIn(FakeEsptool.SAYS, out)
                said = [
                    line.removeprefix(FakeEsptool.SAYS)
                    for line in err.splitlines()
                    if line.startswith(FakeEsptool.SAYS)
                ]
                self.assertEqual(said, heard)

    def test_a_refusal_mid_command_still_resets_the_board(self):
        esptool = FakeEsptool(FLASH)
        self.assertBoardFails(
            esptool,
            ["--partition", "spiffs", "ls"],
            "no partition named 'spiffs'; the table has nvs, otadata, app0, littlefs",
        )
        self.assertEqual(esptool.ops, [*OPEN, TABLE_READ, *CLOSE])

    def test_an_esptool_error_is_reported_and_the_board_reset(self):
        esptool = FakeEsptool(FLASH, fail="read")
        self.assertBoardFails(
            esptool, ["ls"], "error: esptool: Timed out waiting for packet header\n"
        )
        self.assertEqual(esptool.ops, [*OPEN, TABLE_READ, *CLOSE])

    def test_ctrl_c_resets_the_board_and_exits_130(self):
        esptool = FakeEsptool(FLASH, fail="read", error=KeyboardInterrupt())
        self.assertBoardFails(esptool, ["ls"], "", code=130)
        self.assertEqual(esptool.ops, [*OPEN, TABLE_READ, *CLOSE])

    def test_a_failure_while_connecting_still_resets_the_board(self):
        for fail, error, code in (
            ("stub", FatalError("Failed to start stub"), 1),
            ("baud", FatalError("Failed to change baud rate"), 1),
            ("attach", KeyboardInterrupt(), 130),
        ):
            with self.subTest(fail=fail):
                esptool = FakeEsptool(FLASH, fail=fail, error=error)
                self.assertBoardFails(esptool, ["ls"], f"error: esptool: {error}", code)
                at = [op[0] for op in OPEN].index(fail)
                self.assertEqual(esptool.ops, [*OPEN[: at + 1], *CLOSE])

    def test_a_board_that_does_not_answer_has_nothing_to_reset(self):
        esptool = FakeEsptool(FLASH, fail="detect", error=FatalError("No serial data"))
        code, out, err = self.run_board(esptool, "ls")
        self.assertEqual((code, out), (1, ""))
        self.assertEqual(script_lines(err), "error: esptool: No serial data\n")
        self.assertEqual(esptool.ops, [("detect", PORT)])

    def test_a_reset_failing_after_an_error_does_not_hide_it(self):
        # A pulled cable fails the read, then the reset: the read is what to report.
        for fail, error, code, message in (
            (
                "read",
                FatalError("Timed out waiting for packet header"),
                1,
                "error: esptool: Timed out",
            ),
            (
                "stub",
                FatalError("Failed to start stub"),
                1,
                "error: esptool: Failed to start stub",
            ),
            ("read", KeyboardInterrupt(), 130, ""),
        ):
            with self.subTest(fail=fail, error=type(error).__name__):
                esptool = FakeEsptool(FLASH, fail=fail, error=error)
                reset = esptool.reset_chip

                def broken(esp, mode="hard-reset", reset=reset):
                    reset(esp, mode)
                    raise OSError(errno.EIO, "reset: Input/output error")

                esptool.reset_chip = broken
                self.assertBoardFails(esptool, ["ls"], message, code)

    def test_a_reset_that_fails_still_lets_the_port_go(self):
        esptool = FakeEsptool(FLASH, fail="reset")
        code, _, err = self.run_board(esptool, "ls")
        self.assertEqual(code, 1)
        self.assertEqual(
            script_lines(err), "error: esptool: Timed out waiting for packet header\n"
        )
        self.assertEqual(esptool.ops[-2:], CLOSE)


class BoardRead(BoardTestCase):
    def test_a_superblock_claiming_more_than_the_partition_reads_nothing_past_it(self):
        # partition_size shrunk with no reformat: the tail chain runs on past the end,
        # into whatever the flash holds after it.
        fs = LittleFS(block_size=BLOCK_SIZE, block_count=4 * LFS_BLOCKS)
        for i in range(30):
            fs.mkdir(f"/d{i:02}")
        whole = bytes(fs.context.buffer)
        esptool = FakeEsptool(flash_image(whole[:LFS_SIZE]) + whole[LFS_SIZE:])
        code, out, err = self.run_board(esptool, "ls")
        self.assertEqual((code, out), (1, ""))
        self.assertRegex(script_lines(err), "truncated|damaged or cut short")
        end = LFS_OFFSET + LFS_SIZE
        for op in esptool.ops:
            if op[0] == "read" and op != TABLE_READ:
                self.assertTrue(LFS_OFFSET <= op[1] and op[1] + op[2] <= end, op)

    def test_dump_reads_the_table_then_the_whole_partition_with_progress(self):
        esptool = FakeEsptool(FLASH)
        output = self.tmp / "littlefs.bin"
        out = self.run_board_ok(esptool, "dump", output)
        self.assertEqual(
            esptool.ops,
            [*OPEN, TABLE_READ, ("read", LFS_OFFSET, LFS_SIZE, True), *CLOSE],
        )
        self.assertEqual(output.read_bytes(), LFS)
        self.assertEqual(out, f"{LFS_SIZE} bytes to {output}\n")

    def test_a_dump_whose_read_fails_saves_nothing(self):
        output = self.tmp / "littlefs.bin"
        self.assertBoardFails(
            FakeEsptool(FLASH, fail="read", at=2),
            ["dump", output],
            "error: esptool: Timed out waiting for packet header",
        )
        self.assertFalse(output.exists())

    def test_ls_lists_what_it_lists_off_an_image(self):
        out = self.run_board_ok(FakeEsptool(FLASH), "ls")
        self.assertEqual(out, self.run_ok("--image", self.write("f.bin", FLASH), "ls"))

    def test_ls_fetches_only_the_blocks_holding_metadata(self):
        esptool = FakeEsptool(FLASH)
        self.run_board_ok(esptool, "ls")
        reads = [op for op in esptool.ops if op[0] == "read"][1:]
        self.assertTrue(reads)
        for _, offset, size, progress in reads:
            self.assertEqual((size, progress), (BLOCK_SIZE, False))
            self.assertEqual(offset % BLOCK_SIZE, 0)
            self.assertTrue(LFS_OFFSET <= offset < LFS_OFFSET + LFS_SIZE, hex(offset))
        # The superblock pair and one pair per directory; BIG_FILE's blocks are not among them.
        blocks = esptool.blocks_read()
        self.assertEqual(len(set(blocks)), len(blocks))
        self.assertEqual(len(blocks), 2 * (1 + len(DIRS)))

    def test_extract_fetches_each_block_once_and_the_big_files_too(self):
        ls = FakeEsptool(FLASH)
        self.run_board_ok(ls, "ls")
        esptool = FakeEsptool(FLASH)
        dest = self.tmp / "files"
        self.run_board_ok(esptool, "extract", dest)
        blocks = esptool.blocks_read()
        self.assertEqual(len(set(blocks)), len(blocks))
        # What ls read, then BIG_FILE's two blocks.
        self.assertEqual(blocks[: len(ls.blocks_read())], ls.blocks_read())
        self.assertEqual(len(blocks), len(ls.blocks_read()) + 2)
        self.assertEqual(read_tree(dest), TREE)

    def test_a_missing_label_is_refused_before_the_partition_is_read(self):
        esptool = FakeEsptool(FLASH)
        self.assertBoardFails(
            esptool,
            ["--partition", "spiffs", "extract", self.tmp / "files"],
            "no partition named 'spiffs'; the table has nvs, otadata, app0, littlefs",
        )
        self.assertEqual(esptool.ops, [*OPEN, TABLE_READ, *CLOSE])
        self.assertFalse((self.tmp / "files").exists())

    def test_an_erased_partition_has_no_littlefs(self):
        for command in (["ls"], ["extract", self.tmp / "files"]):
            with self.subTest(command=command[0]):
                self.assertBoardFails(
                    FakeEsptool(ERASED_FLASH),
                    command,
                    "error: no LittleFS on the partition: ",
                )
        self.assertFalse((self.tmp / "files").exists())


class BoardLinkLost(BoardTestCase):
    """The link can go at any block; nothing read past that point may be trusted."""

    ERRORS = {
        FatalError("Timed out waiting for packet header"): "esptool: Timed out",
        OSError(errno.EIO, "Input/output error"): "[Errno 5] Input/output error",
    }

    def block_reads(self, command: str) -> int:
        """How many blocks a clean run of ls or extract fetches."""
        esptool = FakeEsptool(FLASH)
        dest = [self.tmp / "clean"] if command == "extract" else []
        self.run_board_ok(esptool, command, *dest)
        return len(esptool.blocks_read())

    def assertNoBadFile(self, dest: Path):
        """What reached DEST is whole, and the file being read when the link went is absent."""
        got = read_tree(dest) if dest.exists() else {}
        for path, data in got.items():
            self.assertEqual(data, TREE[path], path)
        # Every block ahead of BIG_FILE's is fetched at mount, so it is the one being read.
        self.assertNotIn(BIG_FILE, got)

    def test_ls_fails_as_a_whole(self):
        for n in range(1, self.block_reads("ls") + 1):
            for error, message in self.ERRORS.items():
                with self.subTest(block_read=n, error=type(error).__name__):
                    # The table is read 1st.
                    esptool = FakeEsptool(FLASH, fail="read", at=n + 1, error=error)
                    self.assertBoardFails(
                        esptool, ["ls"], f"error: reading the partition: {message}"
                    )

    def test_extract_writes_no_file_it_could_not_read_whole(self):
        # Mount fetches every metadata block, directories' included; then BIG_FILE's blocks.
        for n in range(1, self.block_reads("extract") + 1):
            for error, message in self.ERRORS.items():
                with self.subTest(block_read=n, error=type(error).__name__):
                    dest = self.tmp / f"files-{n}-{type(error).__name__}"
                    esptool = FakeEsptool(FLASH, fail="read", at=n + 1, error=error)
                    self.assertBoardFails(
                        esptool,
                        ["extract", dest],
                        f"error: reading the partition: {message}",
                    )
                    self.assertNoBadFile(dest)

    def test_the_files_read_before_the_link_went_are_kept(self):
        dest = self.tmp / "files"
        last = self.block_reads("extract")
        esptool = FakeEsptool(FLASH, fail="read", at=last + 1)
        self.assertBoardFails(esptool, ["extract", dest], "reading the partition")
        self.assertNoBadFile(dest)
        # Those sorted ahead of BIG_FILE.
        self.assertEqual(
            read_tree(dest), {p: d for p, d in TREE.items() if p < BIG_FILE}
        )

    def test_ctrl_c_on_any_block_exits_130(self):
        for command in ("ls", "extract"):
            for n in range(1, self.block_reads(command) + 1):
                with self.subTest(command=command, block_read=n):
                    dest = self.tmp / f"files-{n}"
                    argv = [command, dest] if command == "extract" else [command]
                    esptool = FakeEsptool(
                        FLASH, fail="read", at=n + 1, error=KeyboardInterrupt()
                    )
                    self.assertBoardFails(esptool, argv, "", code=130)
                    self.assertNoBadFile(dest)


SOURCE_DIRS = ["/logs", "/logs/2026", "/spare"]
SOURCE_FILES = {
    # FLASH has it with other contents: the old file must not show through.
    "/automations.json": b'{"rules": [{"id": 1}]}',
    "/logs/2026/boot.log": b"boot\n",
    "/logs/panic.bin": random.Random(2).randbytes(6000),
    "/settings.json": b'{"relay": "on"}',
}
SOURCE_TREE = {**dict.fromkeys(SOURCE_DIRS), **SOURCE_FILES}
# One per file, so a time stored under the wrong name shows.
SOURCE_MTIMES = {path: MTIME + 3600 * i for i, path in enumerate(SOURCE_FILES, 1)}


def make_source(root: Path) -> Path:
    for path in SOURCE_DIRS:
        (root / path.lstrip("/")).mkdir(parents=True)
    for path, data in SOURCE_FILES.items():
        (root / path.lstrip("/")).write_bytes(data)
        os.utime(root / path.lstrip("/"), (SOURCE_MTIMES[path],) * 2)
    return root


def read_tree(root: Path) -> dict[str, bytes | None]:
    """Every entry under root: a file's bytes, None for a directory."""
    return {
        "/" + p.relative_to(root).as_posix(): None if p.is_dir() else p.read_bytes()
        for p in root.rglob("*")
    }


class WriteImage(TempDirTestCase):
    def setUp(self):
        super().setUp()
        self.source = make_source(self.tmp / "src")

    def extract(self, image: Path, dest: str = "out") -> Path:
        self.run_ok("--image", image, "extract", self.tmp / dest)
        return self.tmp / dest

    def test_the_partition_of_a_whole_flash_image_holds_exactly_the_directory(self):
        image = self.write("flash.bin", FLASH)
        self.run_ok("--image", image, "write", self.source)
        self.assertEqual(read_tree(self.extract(image)), SOURCE_TREE)

    def test_the_files_keep_their_host_mtimes(self):
        image = self.write("flash.bin", FLASH)
        self.run_ok("--image", image, "write", self.source)
        dest = self.extract(image)
        self.assertEqual(
            {p: os.stat(dest / p.lstrip("/")).st_mtime for p in SOURCE_FILES},
            SOURCE_MTIMES,
        )

    def test_nothing_outside_the_partition_changes(self):
        # Flash after the partition too, so a write running past its end shows.
        tail = random.Random(3).randbytes(BLOCK_SIZE)
        image = self.write("flash.bin", FLASH + tail)
        self.run_ok("--image", image, "write", self.source)
        data = image.read_bytes()
        self.assertEqual(len(data), FLASH_SIZE + BLOCK_SIZE)
        self.assertEqual(data[:LFS_OFFSET], FLASH[:LFS_OFFSET])
        self.assertEqual(data[LFS_OFFSET + LFS_SIZE :], tail)

    def test_the_summary_names_the_source_and_the_partition(self):
        out = self.run_ok(
            "--image", self.write("flash.bin", FLASH), "write", self.source
        )
        self.assertEqual(out, f"{self.source} to 'littlefs' at {LFS_OFFSET:#x}\n")

    def test_a_spiffs_subtype_partition_is_written_too(self):
        partitions = [*PARTITIONS[:-1], ("spiffs", DATA, 0x82, LFS_OFFSET, LFS_SIZE)]
        image = self.write("flash.bin", flash_image(LFS, partitions))
        self.run_ok("--image", image, "--partition", "spiffs", "write", self.source)
        self.run_ok(
            "--image", image, "--partition", "spiffs", "extract", self.tmp / "out"
        )
        self.assertEqual(read_tree(self.tmp / "out"), SOURCE_TREE)

    def test_a_raw_dump_is_replaced_whole(self):
        dump = self.write("littlefs.bin", LFS)
        out = self.run_ok("--image", dump, "write", self.source)
        self.assertEqual(out, f"{self.source} to 'littlefs' at 0x0\n")
        self.assertEqual(len(dump.read_bytes()), LFS_SIZE)
        self.assertEqual(read_tree(self.extract(dump)), SOURCE_TREE)

    def test_partition_is_refused_for_a_raw_dump(self):
        dump = self.write("littlefs.bin", LFS)
        self.assertFails(
            ["--image", dump, "--partition", "littlefs", "write", self.source],
            f"{dump} is a partition dump: it has no table to pick 'littlefs' from",
        )
        self.assertEqual(dump.read_bytes(), LFS)

    def test_an_empty_directory_leaves_an_empty_filesystem(self):
        empty = self.tmp / "empty"
        empty.mkdir()
        image = self.write("flash.bin", FLASH)
        self.run_ok("--image", image, "write", empty)
        self.assertEqual(self.run_ok("--image", image, "ls"), "")

    def test_a_saved_dump_is_written_as_is(self):
        saved = self.tmp / "saved.bin"
        self.run_ok("--image", self.write("flash.bin", FLASH), "dump", saved)
        image = self.write("erased.bin", ERASED_FLASH)
        self.run_ok("--image", image, "write", saved)
        # FLASH is ERASED_FLASH with LFS in the partition.
        self.assertEqual(image.read_bytes(), FLASH)

    def test_extract_then_write_gives_the_same_files_back(self):
        first = self.extract(self.write("flash.bin", FLASH), "first")
        # Erased, so whatever is read back came from the write.
        image = self.write("erased.bin", ERASED_FLASH)
        self.run_ok("--image", image, "write", first)
        second = self.extract(image, "second")
        self.assertEqual(read_tree(second), read_tree(first))
        self.assertEqual(os.stat(second / STAMPED.lstrip("/")).st_mtime, MTIME)
        # The rest took the time extract made them at, kept to the second as on the board.
        for path in FILES:
            self.assertEqual(
                os.stat(second / path.lstrip("/")).st_mtime,
                int(os.stat(first / path.lstrip("/")).st_mtime),
                path,
            )


class WriteRefused(TempDirTestCase):
    """The firmware formats a partition it cannot mount, so the image is left as it was."""

    def assertRefused(self, source: Path, message: str, *argv):
        image = self.write("flash.bin", FLASH)
        self.assertFails(["--image", image, *argv, "write", source], message)
        self.assertEqual(image.read_bytes(), FLASH)

    def test_a_file_that_is_not_littlefs_is_refused(self):
        tiny = LittleFS(block_size=BLOCK_SIZE, block_count=LFS_BLOCKS)
        with tiny.open("/a.txt", "wb") as f:
            f.write(b"a")
        for name, data, message in (
            ("erased.bin", b"\xff" * LFS_SIZE, "no LittleFS in {}: "),
            ("random.bin", random.Random(4).randbytes(LFS_SIZE), "no LittleFS in {}: "),
            # A whole-flash image, where only its partition would do.
            ("whole.bin", FLASH, "no LittleFS in {}: "),
            # The superblock is there, the directories it points at are not.
            (
                "damaged.bin",
                LFS[: 3 * BLOCK_SIZE].ljust(LFS_SIZE, b"\xff"),
                "the LittleFS in {} is damaged or cut short: ",
            ),
            # Its metadata all in the superblock pair, so it mounts, short as it is.
            (
                "truncated.bin",
                bytes(tiny.context.buffer)[: 2 * BLOCK_SIZE],
                f"the LittleFS in {{}} is truncated: "
                f"{2 * BLOCK_SIZE} bytes of a {LFS_SIZE}-byte filesystem",
            ),
        ):
            with self.subTest(name=name):
                path = self.write(name, data)
                self.assertRefused(path, message.format(path))

    def test_a_dump_in_a_newer_disk_version_is_refused(self):
        # esp_littlefs mounts disk version 2.1 at most, and formats anything newer.
        newer = LittleFS(block_size=BLOCK_SIZE, block_count=LFS_BLOCKS).fs_stat()
        newer = newer._replace(disk_version=0x00020002)
        with mock.patch.object(LittleFS, "fs_stat", lambda fs: newer):
            path = self.write("newer.bin", LFS)
            self.assertRefused(
                path,
                f"{path}: LittleFS disk version 2.2, newer than the firmware mounts",
            )

    def test_a_directory_is_packed_in_the_firmwares_disk_version(self):
        image = self.write("flash.bin", FLASH)
        self.run_ok("--image", image, "write", make_source(self.tmp / "src"))
        written = LittleFS(
            context=flash_files.image_blocks(image.read_bytes()[LFS_OFFSET:]),
            block_size=BLOCK_SIZE,
            block_count=0,
            mount=True,
        )
        self.assertEqual(written.fs_stat().disk_version, 0x00020001)

    def test_a_dump_of_another_size_is_refused(self):
        bigger = LittleFS(block_size=BLOCK_SIZE, block_count=2 * LFS_BLOCKS)
        smaller = LittleFS(block_size=BLOCK_SIZE, block_count=LFS_BLOCKS // 2)
        for name, data, fs_size in (
            ("bigger.bin", bytes(bigger.context.buffer), 2 * LFS_SIZE),
            # The partition's length, a smaller filesystem inside.
            (
                "smaller.bin",
                bytes(smaller.context.buffer).ljust(LFS_SIZE, b"\xff"),
                LFS_SIZE // 2,
            ),
            # The partition's filesystem, with more after it.
            ("longer.bin", LFS.ljust(2 * LFS_SIZE, b"\xff"), LFS_SIZE),
        ):
            with self.subTest(name=name):
                path = self.write(name, data)
                self.assertRefused(
                    path,
                    f"{path}: a {fs_size}-byte filesystem in {len(data)} bytes, "
                    f"for a {LFS_SIZE}-byte partition",
                )

    def test_a_partition_littlefs_does_not_live_in_is_refused(self):
        source = make_source(self.tmp / "src")
        for label, type_, subtype in (("nvs", DATA, 0x02), ("app0", APP, 0x10)):
            with self.subTest(partition=label):
                self.assertRefused(
                    source,
                    f"'{label}' is not a partition LittleFS lives in "
                    f"(type {type_:#04x}, subtype {subtype:#04x})",
                    "--partition",
                    label,
                )

    def test_a_directory_too_big_is_refused(self):
        source = self.tmp / "src"
        source.mkdir()
        (source / "big.bin").write_bytes(bytes(LFS_SIZE))
        self.assertRefused(source, f"{source} does not fit in {LFS_SIZE} bytes")

    def test_many_small_files_that_do_not_fit_are_refused(self):
        # Space runs out as a file is closed, which must not take the process down.
        source = self.tmp / "src"
        source.mkdir()
        for i in range(200):
            (source / f"{i:03}.txt").write_bytes(bytes([i]) * 300)
        self.assertRefused(source, f"{source} does not fit in {LFS_SIZE} bytes")

    def test_a_link_to_a_directory_is_refused(self):
        # Packed, it would arrive as an empty directory.
        source = self.tmp / "src"
        (source / "real").mkdir(parents=True)
        (source / "real" / "a.txt").write_bytes(b"a")
        try:
            (source / "link").symlink_to(source / "real", target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("no symlinks on this host")
        self.assertRefused(
            source, f"{source / 'link'}: a link to a directory is not packed"
        )

    def test_a_name_littlefs_refuses_is_reported_with_its_path(self):
        # Linux stops a name at 255 bytes as LittleFS does, so a lower limit stands in
        # for a host whose names run longer.
        source = self.tmp / "src"
        (source / "sub").mkdir(parents=True)
        long_name = source / "sub" / ("n" * 17)
        long_name.write_bytes(b"x")
        short = functools.partial(LittleFS, name_max=16)
        with mock.patch.object(flash_files, "LittleFS", short):
            self.assertRefused(
                source, f"{long_name}: LittleFSError -36: LFS_ERR_NAMETOOLONG"
            )

    def test_a_missing_source_is_an_error(self):
        self.assertRefused(self.tmp / "absent", "absent")


class WriteNames(TempDirTestCase):
    """Host names go to LittleFS byte for byte, as extract gives them back."""

    def setUp(self):
        super().setUp()
        self.image = self.write("flash.bin", FLASH)

    def test_names_that_are_not_utf8_keep_their_bytes(self):
        source = os.fsencode(self.tmp / "src")
        os.makedirs(source + b"/d\xe9j\xe0")
        files = [b"/caf\xe9.txt", b"/d\xe9j\xe0/vu.txt"]
        for name in files:
            with open(source + name, "wb") as f:
                f.write(name)
        self.run_ok("--image", self.image, "write", os.fsdecode(source))
        dest = os.fsencode(self.tmp / "files")
        self.run_ok("--image", self.image, "extract", os.fsdecode(dest))
        self.assertEqual(sorted(os.listdir(dest)), [b"caf\xe9.txt", b"d\xe9j\xe0"])
        for name in files:
            with open(dest + name, "rb") as f:
                self.assertEqual(f.read(), name)

    def test_utf8_names_show_in_ls(self):
        source = self.tmp / "src"
        (source / "папка").mkdir(parents=True)
        (source / "папка" / "файл.txt").write_bytes(b"1")
        (source / "привет.txt").write_bytes(b"2")
        self.run_ok("--image", self.image, "write", source)
        out = self.run_ok("--image", self.image, "ls")
        self.assertEqual(
            [LS_LINE.match(line).group(3) for line in out.splitlines()],
            ["/папка/", "/папка/файл.txt", "/привет.txt"],
        )

    def test_a_name_at_the_255_byte_limit_is_kept(self):
        source = self.tmp / "src"
        source.mkdir()
        name = "n" * 251 + ".txt"
        (source / name).write_bytes(b"x")
        self.run_ok("--image", self.image, "write", source)
        self.run_ok("--image", self.image, "extract", self.tmp / "files")
        self.assertEqual(read_tree(self.tmp / "files"), {"/" + name: b"x"})


class BoardWrite(BoardTestCase):
    def setUp(self):
        super().setUp()
        self.source = make_source(self.tmp / "src")

    def test_the_table_is_read_then_the_partition_written_at_its_offset(self):
        esptool = FakeEsptool(FLASH)
        out = self.run_board_ok(esptool, "write", self.source)
        self.assertEqual(
            esptool.ops,
            [*OPEN, TABLE_READ, ("write", LFS_OFFSET, LFS_SIZE), *CLOSE],
        )
        self.assertEqual(out, f"{self.source} to 'littlefs' at {LFS_OFFSET:#x}\n")

    def test_the_write_skips_the_app_image_check(self):
        # A LittleFS block 0 whose revision count starts 0xE9 reads to esptool as an app
        # image header for another chip, and it refuses that without force.
        esptool = FakeEsptool(FLASH)
        self.run_board_ok(esptool, "write", self.source)
        self.assertEqual(esptool.write_options, [{"force": True}])

    def test_a_missing_source_is_refused_before_the_board_is_touched(self):
        esptool = FakeEsptool(FLASH)
        code, out, err = self.run_board(esptool, "write", self.tmp / "absent")
        self.assertEqual((code, out), (1, ""))
        self.assertIn("absent: no such file or directory", script_lines(err))
        self.assertEqual(esptool.ops, [])

    def test_what_is_written_reads_back_as_the_directory(self):
        esptool = FakeEsptool(FLASH)
        self.run_board_ok(esptool, "write", self.source)
        ((offset, written),) = esptool.written
        self.assertEqual((offset, len(written)), (LFS_OFFSET, LFS_SIZE))
        self.run_ok("--image", self.write("w.bin", written), "extract", self.tmp / "a")
        self.assertEqual(read_tree(self.tmp / "a"), SOURCE_TREE)
        # And off the board: the same bytes, where the partition is.
        dump, dest = self.tmp / "littlefs.bin", self.tmp / "b"
        self.run_board_ok(esptool, "dump", dump)
        self.run_board_ok(esptool, "extract", dest)
        self.assertEqual(dump.read_bytes(), written)
        self.assertEqual(read_tree(dest), SOURCE_TREE)

    def test_a_failed_write_is_an_error(self):
        esptool = FakeEsptool(FLASH, fail="write")
        self.assertBoardFails(
            esptool,
            ["write", self.source],
            "error: esptool: Timed out waiting for packet header",
        )
        self.assertEqual(esptool.flash, FLASH)

    def test_a_missing_label_is_refused_before_anything_is_written(self):
        esptool = FakeEsptool(FLASH)
        self.assertBoardFails(
            esptool,
            ["--partition", "spiffs", "write", self.source],
            "no partition named 'spiffs'; the table has nvs, otadata, app0, littlefs",
        )
        self.assertEqual(esptool.ops, [*OPEN, TABLE_READ, *CLOSE])

    def test_a_partition_littlefs_does_not_live_in_is_not_written(self):
        esptool = FakeEsptool(FLASH)
        self.assertBoardFails(
            esptool,
            ["--partition", "nvs", "write", self.source],
            "error: 'nvs' is not a partition LittleFS lives in "
            "(type 0x01, subtype 0x02)",
        )
        self.assertEqual(esptool.ops, [*OPEN, TABLE_READ, *CLOSE])
        self.assertEqual(esptool.flash, FLASH)

    def test_a_source_that_would_not_mount_is_refused_before_anything_is_written(self):
        esptool = FakeEsptool(FLASH)
        erased = self.write("erased.bin", b"\xff" * LFS_SIZE)
        self.assertBoardFails(esptool, ["write", erased], f"no LittleFS in {erased}")
        self.assertEqual(esptool.ops, [*OPEN, TABLE_READ, *CLOSE])


class Arguments(unittest.TestCase):
    def test_exactly_one_source_is_required(self):
        for argv in (["ls"], ["--port", "/dev/ttyUSB0", "--image", "x.bin", "ls"]):
            with (
                self.subTest(argv=argv),
                contextlib.redirect_stderr(io.StringIO()),
                self.assertRaises(SystemExit) as caught,
            ):
                flash_files.main(argv)
            self.assertEqual(caught.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
