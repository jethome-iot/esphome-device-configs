"""The component's YAML schema: the partition sizes it takes, what it refuses, and the defaults.

The C++ is ESP-IDF only, so there is no host build to put cases in; the validators are what
this suite can reach.
"""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_FRAMEWORK,
    KEY_TARGET_PLATFORM,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import CORE

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import littlefs_storage  # noqa: E402

PARTITION_SIZE = littlefs_storage._partition_size


class PartitionSize(unittest.TestCase):
    def test_a_suffix_counts_in_binary_units(self):
        # Unlike cv.validate_bytes, where 4MB is 4_000_000: a partition table is in 4KB pages.
        for value, size in (
            ("4MB", 4 * 1024 * 1024),
            ("512KB", 512 * 1024),
            ("1M", 1024 * 1024),
            ("8K", 8 * 1024),
        ):
            with self.subTest(value=value):
                self.assertEqual(PARTITION_SIZE(value), size)

    def test_a_bare_number_is_bytes(self):
        for value, size in (("4096", 4096), ("4096B", 4096), (0x2000, 0x2000)):
            with self.subTest(value=value):
                self.assertEqual(PARTITION_SIZE(value), size)

    def test_spacing_and_case_are_ignored(self):
        for value in (" 4 mb ", "4Mb", "512 kB"):
            with self.subTest(value=value):
                self.assertEqual(
                    PARTITION_SIZE(value), PARTITION_SIZE(value.strip().upper())
                )

    def test_a_size_that_is_not_a_number_with_a_known_suffix_is_refused(self):
        for value in ("", "4GB", "0x2000", "4 megabytes", "-4096", "4.5MB"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "Expected a size like 4MB"),
            ):
                PARTITION_SIZE(value)

    def test_a_size_below_one_page_is_refused(self):
        for value in ("0", "2KB", 4095, 0):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "value must be at least 4096"),
            ):
                PARTITION_SIZE(value)

    def test_a_size_that_is_not_a_whole_number_of_pages_is_refused(self):
        for value in (5000, "5000", "5000B"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "must be a multiple of 4KB"),
            ):
                PARTITION_SIZE(value)

    def test_a_fractional_number_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "integers with no fractional part"):
            PARTITION_SIZE(1.5)

    def test_anything_but_a_string_or_a_number_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Must be string"):
            PARTITION_SIZE(None)


class Schema(unittest.TestCase):
    # CORE is a process-wide singleton, so the platform this suite needs goes back the way it
    # was: the next module discovered in this directory gets an untouched one.
    def setUp(self):
        self.core = CORE.data.get(KEY_CORE)
        CORE.data[KEY_CORE] = {
            **(self.core or {}),
            KEY_TARGET_PLATFORM: PLATFORM_ESP32,
            KEY_TARGET_FRAMEWORK: "esp-idf",
        }

    def tearDown(self):
        if self.core is None:
            CORE.data.pop(KEY_CORE, None)
        else:
            CORE.data[KEY_CORE] = self.core

    def test_the_defaults_describe_the_partition_the_device_carries(self):
        config = littlefs_storage.CONFIG_SCHEMA({})
        self.assertEqual(
            (
                config[littlefs_storage.CONF_PARTITION_LABEL],
                config[littlefs_storage.CONF_PARTITION_SIZE],
                config[littlefs_storage.CONF_BASE_PATH],
                config[littlefs_storage.CONF_FORMAT_IF_MOUNT_FAILED],
            ),
            ("littlefs", 1024 * 1024, "/littlefs", True),
        )

    def test_a_partition_size_goes_through_the_size_validator(self):
        config = littlefs_storage.CONFIG_SCHEMA({"partition_size": "2MB"})
        self.assertEqual(config[littlefs_storage.CONF_PARTITION_SIZE], 2 * 1024 * 1024)
        with self.assertRaisesRegex(cv.Invalid, "must be a multiple of 4KB"):
            littlefs_storage.CONFIG_SCHEMA({"partition_size": 5000})

    def test_a_partition_label_that_is_not_a_string_is_refused(self):
        # The label names an entry in the partition table; YAML would hand 42 over as an int.
        with self.assertRaisesRegex(cv.Invalid, "Must be string"):
            littlefs_storage.CONFIG_SCHEMA({"partition_label": 42})

    def test_another_platform_is_refused(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_HOST
        with self.assertRaisesRegex(cv.Invalid, "only available on"):
            littlefs_storage.CONFIG_SCHEMA({})

    def test_arduino_is_refused(self):
        # The mount is the ESP-IDF VFS, and the partition is added to the IDF partition table.
        CORE.data[KEY_CORE][KEY_TARGET_FRAMEWORK] = "arduino"
        with self.assertRaisesRegex(cv.Invalid, "only available with framework"):
            littlefs_storage.CONFIG_SCHEMA({})


if __name__ == "__main__":
    unittest.main()
