"""The component's final validation: it needs a switch and a binary_sensor section."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import bindings  # noqa: E402

SWITCHES = [{"platform": "template", "id": "relay_1"}]
INPUTS = [{"platform": "template", "id": "in_1"}]


class FinalValidate(unittest.TestCase):
    def validate(self, full_config):
        token = fv.full_config.set(full_config)
        try:
            return bindings.FINAL_VALIDATE_SCHEMA({})
        finally:
            fv.full_config.reset(token)

    def test_both_sections_present_passes(self):
        self.validate({"bindings": {}, "switch": SWITCHES, "binary_sensor": INPUTS})

    def test_a_missing_switch_section_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "bindings needs a 'switch:' section"):
            self.validate({"bindings": {}, "binary_sensor": INPUTS})

    def test_a_missing_binary_sensor_section_is_refused(self):
        with self.assertRaisesRegex(
            cv.Invalid, "bindings needs a 'binary_sensor:' section"
        ):
            self.validate({"bindings": {}, "switch": SWITCHES})


if __name__ == "__main__":
    unittest.main()
