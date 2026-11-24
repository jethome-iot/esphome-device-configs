"""The component's YAML schema, which this copy keeps as upstream's: its defaults and refusals."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components.ds2484 import one_wire as ds2484  # noqa: E402


class Schema(unittest.TestCase):
    def test_the_default_address_is_0x18_and_both_pullups_are_off(self):
        config = ds2484.CONFIG_SCHEMA({})
        self.assertEqual(config["address"], 0x18)
        self.assertFalse(config["active_pullup"])
        self.assertFalse(config["strong_pullup"])

    def test_a_pullup_that_is_not_a_boolean_is_refused(self):
        for key in ("active_pullup", "strong_pullup"):
            with (
                self.subTest(key=key),
                self.assertRaisesRegex(cv.Invalid, "Expected boolean"),
            ):
                ds2484.CONFIG_SCHEMA({key: "maybe"})
