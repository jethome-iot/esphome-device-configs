"""The component's YAML schema: nothing to configure, and only where there is an app slot."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_PLATFORM,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
)
from esphome.core import CORE

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import firmware_rollback  # noqa: E402


class Schema(unittest.TestCase):
    def setUp(self):
        # only_on() reads the target platform; the suite builds for host.
        CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def test_a_bare_key_is_enough(self):
        self.assertEqual(firmware_rollback.CONFIG_SCHEMA({}), {})

    def test_the_device_platform_is_accepted(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        self.assertEqual(firmware_rollback.CONFIG_SCHEMA({}), {})

    def test_a_platform_without_esp_ota_is_refused(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP8266
        with self.assertRaisesRegex(cv.Invalid, "only available on"):
            firmware_rollback.CONFIG_SCHEMA({})

    def test_an_option_is_refused(self):
        for key in ("partition", "verify"):
            with (
                self.subTest(key=key),
                self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"),
            ):
                firmware_rollback.CONFIG_SCHEMA({key: "x"})


if __name__ == "__main__":
    unittest.main()
