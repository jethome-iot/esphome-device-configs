"""The component's YAML schema: what it accepts, what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_PLATFORM,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
)
from esphome.core import CORE

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import climate_hub  # noqa: E402


def setUpModule():
    # only_on() reads the target platform; the suite builds for host.
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST


class Defaults(unittest.TestCase):
    def test_a_storage_is_enough(self):
        config = climate_hub.CONFIG_SCHEMA({"storage": "user_storage"})
        self.assertEqual(config[climate_hub.CONF_FOLDER_PATH], "climates")
        self.assertEqual(config[climate_hub.CONF_MAX_CONTROLLERS], 8)
        self.assertNotIn("web_server", config)

    def test_the_storage_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            climate_hub.CONFIG_SCHEMA({})

    def test_no_sensor_or_switch_is_pulled_in(self):
        # Without them the hub finds nothing to bind, as automations does.
        self.assertEqual(climate_hub.AUTO_LOAD, ["climate", "json", "loop_job"])
        self.assertEqual(climate_hub.DEPENDENCIES, ["filesystem_storage_abstract"])


class FolderPath(unittest.TestCase):
    def test_a_folder_name_passes(self):
        self.assertEqual(climate_hub.folder_name("thermostats"), "thermostats")

    def test_anything_but_a_folder_name_is_refused(self):
        for value in ("", "a/b", "/abs", "..", ".", "a\\b"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "folder_path must be a single folder name"
                ),
            ):
                climate_hub.folder_name(value)


class MaxControllers(unittest.TestCase):
    def test_the_range_is_one_to_sixteen(self):
        for value in (1, 16):
            config = climate_hub.CONFIG_SCHEMA(
                {"storage": "user_storage", "max_controllers": value}
            )
            self.assertEqual(config[climate_hub.CONF_MAX_CONTROLLERS], value)

    def test_outside_the_range_is_refused(self):
        for value, message in ((0, "at least 1"), (17, "at most 16")):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, message),
            ):
                climate_hub.CONFIG_SCHEMA(
                    {"storage": "user_storage", "max_controllers": value}
                )


class Platform(unittest.TestCase):
    def test_other_platforms_are_refused(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP8266
        try:
            with self.assertRaisesRegex(
                cv.Invalid, r"This feature is only available on \[.*'esp32'.*'host'.*\]"
            ):
                climate_hub.CONFIG_SCHEMA({"storage": "user_storage"})
        finally:
            CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_HOST


class FinalValidate(unittest.TestCase):
    def validate(self, full_config):
        token = fv.full_config.set(full_config)
        try:
            return climate_hub.FINAL_VALIDATE_SCHEMA({})
        finally:
            fv.full_config.reset(token)

    def test_a_web_server_that_hides_internal_entities_passes(self):
        self.validate({"web_server": {"include_internal": False}})
        self.validate({})

    def test_a_web_server_listing_internal_entities_is_refused(self):
        # It would list the unused thermostat slots, every one under the same placeholder name.
        with self.assertRaisesRegex(
            cv.Invalid,
            "climate_hub keeps its unused thermostat entities internal; "
            "web_server include_internal: true would list them",
        ):
            self.validate({"web_server": {"include_internal": True}})


if __name__ == "__main__":
    unittest.main()
