"""scripts/flash-files.py: finding the user partition in an image or on a board, and reading it."""

import contextlib
import hashlib
import importlib.util
import io
import os
import random
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from datetime import datetime
from pathlib import Path
from unittest import mock

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


def flash_image(littlefs: bytes) -> bytes:
    flash = bytearray(b"\xff" * FLASH_SIZE)
    flash[0x8000 : 0x8000 + flash_files.TABLE_SIZE] = partition_table(PARTITIONS)
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
            [(p.name, p.offset, p.size) for p in parsed],
            [(name, offset, size) for name, _, _, offset, size in PARTITIONS],
        )

    def test_a_table_without_md5_ends_at_the_erased_tail(self):
        parsed = flash_files.parse_partition_table(
            partition_table(PARTITIONS, md5=False)
        )
        self.assertEqual([p.name for p in parsed], [p[0] for p in PARTITIONS])

    def test_a_partition_is_found_by_name(self):
        partition = flash_files.find_partition(partition_table(PARTITIONS), "nvs")
        self.assertEqual(partition, flash_files.Partition("nvs", 0x9000, 0x5000))

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
                    f"truncated: {2 * BLOCK_SIZE} bytes of a {LFS_SIZE}-byte filesystem",
                )

    def test_a_filesystem_that_does_not_mount_is_called_damaged(self):
        # The superblock is there, the directories it points at are not.
        path = self.write("littlefs.bin", LFS[: 3 * BLOCK_SIZE])
        self.assertFails(
            ["--image", path, "ls"], "the LittleFS is damaged or cut short"
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
        self.assertEqual(sorted(paths), ["/caf\ufffd.txt", "/привет.txt"])

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


class FakeEsptool:
    """Stands in for `python -m esptool read-flash`: copies the slice out of a flash image."""

    def __init__(self, flash: bytes, fail_call: int = 0, interrupt_call: int = 0):
        self.flash = flash
        self.fail_call = fail_call
        self.interrupt_call = interrupt_call
        self.calls = []
        self.to_stderr = []

    def __call__(self, cmd, *args, **kwargs):
        self.calls.append(cmd)
        self.to_stderr.append(kwargs.get("stdout") is sys.stderr)
        if len(self.calls) == self.interrupt_call:
            raise KeyboardInterrupt
        if len(self.calls) == self.fail_call:
            return subprocess.CompletedProcess(cmd, 2)
        offset, size, output = self.read_args(cmd)
        start = int(offset, 16)
        Path(output).write_bytes(self.flash[start : start + int(size, 16)])
        return subprocess.CompletedProcess(cmd, 0)

    @staticmethod
    def read_args(cmd) -> list[str]:
        at = cmd.index("read-flash")
        return cmd[at + 1 : at + 4]

    def reads(self) -> list[tuple[str, str, str]]:
        """(offset, size, --after) of every call."""
        return [
            (*self.read_args(cmd)[:2], cmd[cmd.index("--after") + 1])
            for cmd in self.calls
        ]


class BoardSource(TempDirTestCase):
    PORT = "/dev/ttyUSB7"

    def run_board(self, esptool: FakeEsptool, *argv):
        with mock.patch.object(flash_files.subprocess, "run", side_effect=esptool):
            return run_main("--port", self.PORT, *argv)

    def assertBoardFails(self, esptool: FakeEsptool, argv, message: str):
        with mock.patch.object(flash_files.subprocess, "run", side_effect=esptool):
            self.assertFails(["--port", self.PORT, *argv], message)

    def test_the_table_then_the_partition_each_read_ending_in_a_reset(self):
        esptool = FakeEsptool(FLASH)
        output = self.tmp / "littlefs.bin"
        code, _, err = self.run_board(esptool, "--baud", "115200", "dump", output)
        self.assertEqual((code, err), (0, ""))
        self.assertEqual(
            esptool.reads(),
            [
                ("0x8000", "0xc00", "hard-reset"),
                (hex(LFS_OFFSET), hex(LFS_SIZE), "hard-reset"),
            ],
        )
        # stdout carries the listing; esptool's progress goes to stderr.
        self.assertEqual(esptool.to_stderr, [True, True])
        for cmd in esptool.calls:
            self.assertEqual(cmd[:3], [sys.executable, "-m", "esptool"])
            self.assertEqual(cmd[cmd.index("--port") + 1], self.PORT)
            self.assertEqual(cmd[cmd.index("--baud") + 1], "115200")
        self.assertEqual(output.read_bytes(), LFS)

    def test_the_files_on_the_board_are_listed(self):
        code, out, err = self.run_board(FakeEsptool(FLASH), "ls")
        self.assertEqual((code, err), (0, ""))
        for path in FILES:
            self.assertIn(path, out)

    def test_a_failed_esptool_read_is_an_error(self):
        for call, message in (
            (1, "esptool could not read 0xc00 bytes at 0x8000"),
            (2, f"esptool could not read {LFS_SIZE:#x} bytes at {LFS_OFFSET:#x}"),
        ):
            with self.subTest(call=call):
                esptool = FakeEsptool(FLASH, fail_call=call)
                self.assertBoardFails(esptool, ["ls"], message)
                self.assertEqual(len(esptool.calls), call)

    def test_a_missing_label_is_refused_before_the_partition_is_read(self):
        esptool = FakeEsptool(FLASH)
        self.assertBoardFails(
            esptool,
            ["--partition", "spiffs", "ls"],
            "no partition named 'spiffs'; the table has nvs, otadata, app0, littlefs",
        )
        # The one read has already put the board back into its firmware.
        self.assertEqual(esptool.reads(), [("0x8000", "0xc00", "hard-reset")])

    def test_an_erased_partition_has_no_littlefs(self):
        for command in (["ls"], ["extract", self.tmp / "files"]):
            with self.subTest(command=command[0]):
                self.assertBoardFails(
                    FakeEsptool(ERASED_FLASH), command, "no LittleFS on the partition"
                )
        self.assertFalse((self.tmp / "files").exists())

    def test_ctrl_c_exits_130(self):
        code, _, err = self.run_board(FakeEsptool(FLASH, interrupt_call=2), "ls")
        self.assertEqual((code, err), (130, ""))


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
