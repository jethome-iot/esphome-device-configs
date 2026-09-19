"""The component's YAML schema: what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import automations  # noqa: E402


class FolderPath(unittest.TestCase):
    def test_a_folder_name_passes(self):
        self.assertEqual(automations.folder_name("rules"), "rules")

    def test_anything_but_a_folder_name_is_refused(self):
        for value in ("", "a/b", "/abs", "..", ".", "a\\b"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "folder_path must be a single folder name"
                ),
            ):
                automations.folder_name(value)

    def test_the_default_is_a_folder_name(self):
        config = automations.CONFIG_SCHEMA({"storage": "user_storage"})
        self.assertEqual(config[automations.CONF_FOLDER_PATH], "automations")


if __name__ == "__main__":
    unittest.main()
