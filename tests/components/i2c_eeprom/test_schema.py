"""The component's YAML schema: the sizes it knows and what it refuses."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import i2c_eeprom  # noqa: E402


class Schema(unittest.TestCase):
    def test_size_is_the_part_name_in_kilobits(self):
        for name, size in (
            ("64KB", 8192),
            ("64kb", 8192),
            ("1KB", 128),
            ("512KB", 65536),
        ):
            with self.subTest(name=name):
                config = i2c_eeprom.CONFIG_SCHEMA({"size": name})
                self.assertEqual(config["size"], name.upper())
                self.assertEqual(config["size"].enum_value, size)

    def test_the_default_address_is_0x50(self):
        config = i2c_eeprom.CONFIG_SCHEMA({"size": "64KB"})
        self.assertEqual(config["address"], 0x50)

    def test_an_unknown_size_is_refused(self):
        for value in ("3KB", "8192", "64"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "Unknown value"),
            ):
                i2c_eeprom.CONFIG_SCHEMA({"size": value})

    def test_the_default_page_size_is_the_smallest_any_24cxx_uses(self):
        config = i2c_eeprom.CONFIG_SCHEMA({"size": "64KB"})
        self.assertEqual(config["page_size"], 8)

    def test_a_named_page_size_is_taken(self):
        for value in (1, 16, 32):
            with self.subTest(value=value):
                config = i2c_eeprom.CONFIG_SCHEMA({"size": "64KB", "page_size": value})
                self.assertEqual(config["page_size"], value)

    def test_a_page_size_that_is_not_a_power_of_two_is_refused(self):
        for value in (3, 12, 100):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "page_size must be a power of two"),
            ):
                i2c_eeprom.CONFIG_SCHEMA({"size": "64KB", "page_size": value})

    def test_a_page_size_outside_the_range_is_refused(self):
        # 128 is the largest page anywhere in this range of parts, whatever the density.
        for value in (0, -8, 256, 512):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "value must be at (least|most)"),
            ):
                i2c_eeprom.CONFIG_SCHEMA({"size": "512KB", "page_size": value})

    def test_a_page_larger_than_the_density_has_is_refused(self):
        # A page that is not the chip's is worse than a smaller one: the write rolls over
        # inside the transaction instead of being split.
        for name, value in (("1KB", 32), ("64KB", 64), ("256KB", 128)):
            with (
                self.subTest(name=name),
                self.assertRaisesRegex(cv.Invalid, "the largest page a " + name),
            ):
                i2c_eeprom.CONFIG_SCHEMA({"size": name, "page_size": value})

    def test_the_largest_page_each_density_has_is_taken(self):
        for name, value in (("1KB", 16), ("64KB", 32), ("256KB", 64), ("512KB", 128)):
            with self.subTest(name=name):
                config = i2c_eeprom.CONFIG_SCHEMA({"size": name, "page_size": value})
                self.assertEqual(config["page_size"], value)

    def test_a_boolean_page_size_is_refused(self):
        # A YAML `true` arrives as a Python bool, which is an int and would pass as 1.
        for value in (True, False):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "must be a number of bytes"),
            ):
                i2c_eeprom.CONFIG_SCHEMA({"size": "64KB", "page_size": value})

    def test_size_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            i2c_eeprom.CONFIG_SCHEMA({"address": 0x54})


if __name__ == "__main__":
    unittest.main()
