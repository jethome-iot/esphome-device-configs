"""The component's YAML schema: an id and nothing else, only where there is an app slot, and
the actions and the condition that drive it."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import automation, loader
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

    def test_a_bare_key_gets_an_instance(self):
        config = firmware_rollback.CONFIG_SCHEMA({})
        self.assertEqual(list(config), ["id"])
        self.assertTrue(config["id"].is_declaration)
        self.assertEqual(config["id"].type, firmware_rollback.FirmwareRollback)

    def test_it_takes_an_id(self):
        config = firmware_rollback.CONFIG_SCHEMA({"id": "rollback"})
        self.assertEqual(config["id"].id, "rollback")

    def test_the_device_platform_is_accepted(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        self.assertEqual(list(firmware_rollback.CONFIG_SCHEMA({})), ["id"])

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

    def test_a_malformed_id_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "cannot be a digit"):
            firmware_rollback.CONFIG_SCHEMA({"id": "9rollback"})


def action(name, value):
    return automation.ACTION_REGISTRY[f"firmware_rollback.{name}"].schema(value)


def condition(value):
    return automation.CONDITION_REGISTRY["firmware_rollback.is_available"].schema(value)


# What each of the three takes and refuses alike: the instance's id, on its own or as a key.
SCHEMAS = {
    "refresh": lambda value: action("refresh", value),
    "rollback": lambda value: action("rollback", value),
    "is_available": condition,
}


class Automations(unittest.TestCase):
    def test_they_are_registered(self):
        self.assertIn("firmware_rollback.refresh", automation.ACTION_REGISTRY)
        self.assertIn("firmware_rollback.rollback", automation.ACTION_REGISTRY)
        self.assertIn("firmware_rollback.is_available", automation.CONDITION_REGISTRY)

    def test_each_takes_the_id_on_its_own(self):
        for name, schema in SCHEMAS.items():
            with self.subTest(name=name):
                config = schema("rollback")
                self.assertEqual(config["id"].id, "rollback")
                self.assertFalse(config["id"].is_declaration)

    def test_each_takes_the_id_as_a_key(self):
        for name, schema in SCHEMAS.items():
            with self.subTest(name=name):
                self.assertEqual(schema({"id": "rollback"})["id"].id, "rollback")

    # The one instance, as upstream's actions find a component declared once.
    def test_each_works_without_an_id(self):
        for name, schema in SCHEMAS.items():
            with self.subTest(name=name):
                config = schema({})
                self.assertIsNone(config["id"].id)
                self.assertEqual(config["id"].type, firmware_rollback.FirmwareRollback)

    def test_each_refuses_an_unknown_key(self):
        for name, schema in SCHEMAS.items():
            with (
                self.subTest(name=name),
                self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"),
            ):
                schema({"id": "rollback", "partition": "app1"})

    def test_each_refuses_a_malformed_id(self):
        for name, schema in SCHEMAS.items():
            with (
                self.subTest(name=name),
                self.assertRaisesRegex(cv.Invalid, "cannot be a digit"),
            ):
                schema("9rollback")

    def test_only_rollback_takes_on_error(self):
        for name in ("refresh", "is_available"):
            with (
                self.subTest(name=name),
                self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"),
            ):
                SCHEMAS[name]({"id": "rollback", "on_error": [{"delay": "1s"}]})

    def test_rollback_takes_an_on_error_automation(self):
        config = action("rollback", {"id": "rollback", "on_error": [{"delay": "1s"}]})
        then = config["on_error"]["then"]
        self.assertEqual(len(then), 1)
        self.assertIn("delay", then[0])

    def test_rollback_refuses_an_unknown_action_in_on_error(self):
        with self.assertRaisesRegex(cv.Invalid, "Unable to find action"):
            action("rollback", {"id": "rollback", "on_error": [{"reboot_now": {}}]})

    def test_rollback_refuses_an_on_error_that_is_no_automation(self):
        with self.assertRaisesRegex(cv.Invalid, "expected a dictionary"):
            action("rollback", {"id": "rollback", "on_error": "Confirm - failed"})


if __name__ == "__main__":
    unittest.main()
