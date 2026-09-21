"""The component's YAML schema: what it needs and what it refuses."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import jethome_board_info  # noqa: E402


class Schema(unittest.TestCase):
    def test_the_eeprom_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            jethome_board_info.CONFIG_SCHEMA({})

    def test_a_complete_config_passes(self):
        config = jethome_board_info.CONFIG_SCHEMA({"eeprom_id": "eeprom_cpu"})
        self.assertEqual(
            str(config[jethome_board_info.CONF_EEPROM_ID].id), "eeprom_cpu"
        )

    def test_the_eeprom_is_write_protected_by_default(self):
        config = jethome_board_info.CONFIG_SCHEMA({"eeprom_id": "eeprom_cpu"})
        self.assertIs(config[jethome_board_info.CONF_PROTECT_EEPROM], True)

    def test_the_protection_can_be_turned_off(self):
        config = jethome_board_info.CONFIG_SCHEMA(
            {"eeprom_id": "eeprom_cpu", "protect_eeprom": "no"}
        )
        self.assertIs(config[jethome_board_info.CONF_PROTECT_EEPROM], False)

    def test_a_protection_that_is_not_a_boolean_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Expected boolean"):
            jethome_board_info.CONFIG_SCHEMA(
                {"eeprom_id": "eeprom_cpu", "protect_eeprom": "sometimes"}
            )

    def test_unknown_keys_are_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "address"):
            jethome_board_info.CONFIG_SCHEMA(
                {"eeprom_id": "eeprom_cpu", "address": 0x54}
            )


if __name__ == "__main__":
    unittest.main()
