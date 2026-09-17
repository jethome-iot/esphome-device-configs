"""The component's YAML schema: what it accepts, what it refuses, and with which message."""

import importlib
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

from esphome.components import web_device_dashboard as dashboard  # noqa: E402
from esphome.components.web_server_base import (  # noqa: E402
    CONF_WEB_SERVER_BASE_ID,
)


def setUpModule():
    # only_on() reads the target platform; the suite builds for host.
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST


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

    def test_the_board_and_the_storage_are_optional(self):
        config = dashboard.CONFIG_SCHEMA({})
        self.assertNotIn(dashboard.CONF_BOARD_INFO_ID, config)
        self.assertNotIn(dashboard.CONF_STORAGE_ID, config)

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


class StorageId(unittest.TestCase):
    """What a factory reset wipes, and what /capabilities reports as the device's storage."""

    def test_it_resolves_to_any_filesystem_storage(self):
        # The base class, not littlefs_storage: the dashboard only calls format() and the
        # info getters, and the host suite stands a directory in.
        config = dashboard.CONFIG_SCHEMA({"storage_id": "user_storage"})
        storage = config[dashboard.CONF_STORAGE_ID]
        self.assertEqual(storage.id, "user_storage")
        self.assertEqual(
            storage.type, "filesystem_storage_abstract::FilesystemStorageAbstract"
        )
        self.assertFalse(storage.is_declaration)

    def test_what_is_not_an_id_is_refused(self):
        for value in (42, "not a name!", "9lives"):
            with self.subTest(value=value), self.assertRaises(cv.Invalid):
                dashboard.CONFIG_SCHEMA({"storage_id": value})


class ServedScreens(unittest.TestCase):
    """The prefixes /capabilities reports come out of the other components' own configs."""

    CONFIGS = {"web_file_browser": {"storage_id": "store"}, "web_automation_editor": {}}

    def test_each_one_is_configured_under_the_key_the_dashboard_looks_up(self):
        self.assertEqual(sorted(dashboard.SERVED_BY), sorted(self.CONFIGS))
        for name, extra in self.CONFIGS.items():
            with self.subTest(component=name):
                module = importlib.import_module(f"esphome.components.{name}")
                config = module.CONFIG_SCHEMA(extra)
                self.assertIn(dashboard.CONF_URL_PREFIX, config)

    def test_the_component_has_the_setter_to_code_calls(self):
        # cg.MockObj answers to any attribute, so a renamed setter is otherwise a link error
        # in a full firmware build and nothing here.
        header = (
            Path(__file__).resolve().parents[3]
            / "components/web_device_dashboard/web_device_dashboard.h"
        ).read_text()
        for setter in dashboard.SERVED_BY.values():
            with self.subTest(setter=setter):
                self.assertIn(f"void {setter}(", header)

    def test_a_prefix_is_normalized_before_the_dashboard_reads_it(self):
        # to_code would otherwise be the only place the leading slash is added, and the
        # dashboard never gets there: it reads the validated config.
        for name, extra in self.CONFIGS.items():
            with self.subTest(component=name):
                module = importlib.import_module(f"esphome.components.{name}")
                config = module.CONFIG_SCHEMA({**extra, "url_prefix": "somewhere/"})
                self.assertEqual(config[dashboard.CONF_URL_PREFIX], "/somewhere")

    def test_a_prefix_that_is_not_a_path_below_the_root_is_refused(self):
        for value in ("", "/", "with space", "q?x"):
            for name, extra in self.CONFIGS.items():
                module = importlib.import_module(f"esphome.components.{name}")
                with (
                    self.subTest(component=name, value=value),
                    self.assertRaises(cv.Invalid),
                ):
                    module.CONFIG_SCHEMA({**extra, "url_prefix": value})


class UnknownKeys(unittest.TestCase):
    def test_a_misspelled_option_is_refused(self):
        for key in ("board_info", "boardinfo_id", "url_prefix", "storage"):
            with (
                self.subTest(key=key),
                self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"),
            ):
                dashboard.CONFIG_SCHEMA({key: "x"})


class Platforms(unittest.TestCase):
    def tearDown(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def test_the_device_platform_is_accepted(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        self.assertIn("id", dashboard.CONFIG_SCHEMA({}))

    def test_a_platform_the_handler_has_no_server_for_is_refused(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP8266
        with self.assertRaisesRegex(cv.Invalid, "only available on"):
            dashboard.CONFIG_SCHEMA({})


if __name__ == "__main__":
    unittest.main()
