"""The component's YAML schema: the folder name and the defaults."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import config_json  # noqa: E402


class ConfigDir(unittest.TestCase):
    def test_a_folder_name_passes(self):
        self.assertEqual(config_json.folder_name("settings"), "settings")

    def test_anything_but_a_folder_name_is_refused(self):
        for value in ("", "a/b", "/abs", "..", ".", "a\\b", "../x"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "config_dir must be a single folder name"
                ),
            ):
                config_json.folder_name(value)

    def test_the_defaults(self):
        config = config_json.CONFIG_SCHEMA({"storage": "user_storage"})
        self.assertEqual(config[config_json.CONF_CONFIG_DIR], "config")
        self.assertEqual(config[config_json.CONF_SAVE_DELAY].total_milliseconds, 10000)


if __name__ == "__main__":
    unittest.main()
