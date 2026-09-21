"""The component's YAML schema: what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import web_origin_guard as guard  # noqa: E402


class Schema(unittest.TestCase):
    def test_an_empty_block_is_enough(self):
        # How it is always configured: auto-loaded, with no block of its own in any YAML.
        config = guard.CONFIG_SCHEMA({})
        self.assertIn("id", config)
        self.assertIn(guard.CONF_WEB_SERVER_BASE_ID, config)

    def test_an_unknown_key_is_refused(self):
        # Nothing here is configurable, the allowed origins least of all: the whole point is one
        # rule for every handler on the port.
        for key in ("allowed_origins", "origin", "enabled"):
            with (
                self.subTest(key=key),
                self.assertRaisesRegex(cv.Invalid, "Invalid|extra keys"),
            ):
                guard.CONFIG_SCHEMA({key: "anything"})


if __name__ == "__main__":
    unittest.main()
