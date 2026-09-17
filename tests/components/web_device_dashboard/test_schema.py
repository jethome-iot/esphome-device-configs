"""The component's YAML schema: what it accepts, what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import web_device_dashboard as dashboard  # noqa: E402
from esphome.components.web_server_base import (  # noqa: E402
    CONF_WEB_SERVER_BASE_ID,
)


class MinimalConfig(unittest.TestCase):
    def test_an_empty_config_is_enough(self):
        config = dashboard.CONFIG_SCHEMA({})
        self.assertEqual(config["id"].type, "web_device_dashboard::WebDeviceDashboard")
        self.assertTrue(config["id"].is_declaration)

    def test_the_server_is_found_without_being_named(self):
        config = dashboard.CONFIG_SCHEMA({})
        server = config[CONF_WEB_SERVER_BASE_ID]
        self.assertEqual(server.type, "web_server_base::WebServerBase")
        self.assertFalse(server.is_declaration)

    def test_the_board_is_optional(self):
        self.assertNotIn(dashboard.CONF_BOARD_INFO_ID, dashboard.CONFIG_SCHEMA({}))

    def test_the_page_needs_a_server_to_hang_off(self):
        # Why the host suite stands web_server in: upstream builds it for ESP platforms only.
        self.assertEqual(dashboard.DEPENDENCIES, ["web_server_base", "web_server"])


class BoardInfoId(unittest.TestCase):
    def test_it_resolves_to_the_board_component(self):
        config = dashboard.CONFIG_SCHEMA({"board_info_id": "my_board"})
        board = config[dashboard.CONF_BOARD_INFO_ID]
        self.assertEqual(board.id, "my_board")
        self.assertEqual(board.type, "jethome_board_info::JetHomeBoardInfo")
        # A reference, not a declaration: the board is declared by jethome_board_info:.
        self.assertFalse(board.is_declaration)

    def test_what_is_not_an_id_is_refused(self):
        for value in (42, "not a name!", "9lives"):
            with self.subTest(value=value), self.assertRaises(cv.Invalid):
                dashboard.CONFIG_SCHEMA({"board_info_id": value})


class UnknownKeys(unittest.TestCase):
    def test_a_misspelled_option_is_refused(self):
        for key in ("board_info", "boardinfo_id", "url_prefix"):
            with (
                self.subTest(key=key),
                self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"),
            ):
                dashboard.CONFIG_SCHEMA({key: "x"})


if __name__ == "__main__":
    unittest.main()
