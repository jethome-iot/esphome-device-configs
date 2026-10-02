"""The component's YAML schema: the folder name, the count kept and the platforms."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_FRAMEWORK,
    KEY_TARGET_PLATFORM,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
)
from esphome.core import CORE

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import crash_report  # noqa: E402


class ReportDir(unittest.TestCase):
    def test_a_folder_name_passes(self):
        self.assertEqual(crash_report.folder_name("crash"), "crash")
        self.assertEqual(crash_report.folder_name("crash.old"), "crash.old")

    def test_anything_but_a_folder_name_is_refused(self):
        for value in ("", "/", "a/b", "/abs", ".", "..", "a\\b", "../x"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "report_dir must be a single folder name"
                ),
            ):
                crash_report.folder_name(value)

    # /files refuses any path holding "..", so the reports would be out of reach.
    def test_a_name_holding_two_dots_is_refused(self):
        for value in ("a..b", "..x", "x.."):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid,
                    "report_dir cannot contain '..': web_file_browser rejects such a path",
                ),
            ):
                crash_report.folder_name(value)


class Schema(unittest.TestCase):
    def setUp(self):
        # only_on() reads the target platform; the suite builds for host.
        CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def test_the_defaults(self):
        config = crash_report.CONFIG_SCHEMA({"storage": "user_storage"})
        self.assertEqual(config[crash_report.CONF_REPORT_DIR], "crash")
        self.assertEqual(config[crash_report.CONF_KEEP], 4)

    def test_a_full_config_passes(self):
        config = crash_report.CONFIG_SCHEMA(
            {"storage": "user_storage", "report_dir": "panics", "keep": 16}
        )
        self.assertEqual(config[crash_report.CONF_REPORT_DIR], "panics")
        self.assertEqual(config[crash_report.CONF_KEEP], 16)

    def test_the_storage_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            crash_report.CONFIG_SCHEMA({})

    def test_keep_is_one_to_sixteen(self):
        for keep, message in ((0, "must be at least 1"), (17, "must be at most 16")):
            with (
                self.subTest(keep=keep),
                self.assertRaisesRegex(cv.Invalid, message),
            ):
                crash_report.CONFIG_SCHEMA({"storage": "user_storage", "keep": keep})
        for keep in (1, 16):
            with self.subTest(keep=keep):
                crash_report.CONFIG_SCHEMA({"storage": "user_storage", "keep": keep})

    def test_only_esp32_and_the_host_suite(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        CORE.data[KEY_CORE][KEY_TARGET_FRAMEWORK] = "esp-idf"
        crash_report.CONFIG_SCHEMA({"storage": "user_storage"})
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP8266
        with self.assertRaisesRegex(cv.Invalid, "only available on"):
            crash_report.CONFIG_SCHEMA({"storage": "user_storage"})

    # Under Arduino ESPHome keeps no crash record, so the component would never write.
    def test_arduino_is_refused(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        CORE.data[KEY_CORE][KEY_TARGET_FRAMEWORK] = "arduino"
        with self.assertRaisesRegex(cv.Invalid, "needs the esp-idf framework"):
            crash_report.CONFIG_SCHEMA({"storage": "user_storage"})


if __name__ == "__main__":
    unittest.main()
