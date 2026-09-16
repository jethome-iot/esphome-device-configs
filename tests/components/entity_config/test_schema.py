"""The component's YAML schema: what it refuses, and what a disabled type leaves behind."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader
from esphome.const import KEY_CORE, KEY_TARGET_PLATFORM, PLATFORM_HOST
from esphome.core import CORE

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import entity_config  # noqa: E402


class Settings(unittest.TestCase):
    def setUp(self):
        # only_on() reads the target platform; the suite builds for host.
        CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def test_both_types_are_on_by_default(self):
        config = entity_config.CONFIG_SCHEMA({})
        self.assertEqual(
            config[entity_config.CONF_SETTINGS], ["switch", "binary_sensor"]
        )
        self.assertIn(entity_config.CONF_SWITCH_APPLY_ID, config)
        self.assertIn(entity_config.CONF_BINARY_SENSOR_APPLY_ID, config)

    def test_a_disabled_type_reserves_no_component_slot(self):
        config = entity_config.CONFIG_SCHEMA({"settings": ["switch"]})
        self.assertIn(entity_config.CONF_SWITCH_APPLY_ID, config)
        self.assertNotIn(entity_config.CONF_BINARY_SENSOR_APPLY_ID, config)
        # The settings id stays declared: a lambda may still name it, and fails at link.
        self.assertIn(entity_config.CONF_BINARY_SENSOR_SETTINGS_ID, config)

    def test_an_unknown_type_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Unknown value 'uart'"):
            entity_config.CONFIG_SCHEMA({"settings": ["uart"]})


if __name__ == "__main__":
    unittest.main()
