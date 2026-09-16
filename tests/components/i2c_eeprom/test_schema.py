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

    def test_size_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            i2c_eeprom.CONFIG_SCHEMA({"address": 0x54})


if __name__ == "__main__":
    unittest.main()
