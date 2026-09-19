"""The component's YAML schema: what it refuses, and with which message.

The C++ includes <esp_http_server.h> unconditionally, so there is no host build to
put the cases in; the validators are what this suite can reach.
"""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader
from esphome.const import CONF_HEIGHT, CONF_WIDTH

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components.virtual_display import display as virtual_display  # noqa: E402

KEY_NAME = virtual_display._validate_key_name
URL_PREFIX = virtual_display._validate_url_prefix


class KeyName(unittest.TestCase):
    def test_a_name_is_kept(self):
        for value in ("up", "KEY_1", "page-down", "_", "0"):
            with self.subTest(value=value):
                self.assertEqual(KEY_NAME(value), value)

    def test_a_name_that_is_not_one_path_segment_is_refused(self):
        for value in (
            "",
            "up\n",  # fullmatch, not '$': a trailing newline is not a name
            "up down",
            "up\t",
            ".",
            "up/down",
            "%75p",
            "вверх",
        ):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "must be non-empty and use only letters"
                ),
            ):
                KEY_NAME(value)

    def test_anything_but_a_string_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Must be string"):
            KEY_NAME(42)


class UrlPrefix(unittest.TestCase):
    def test_a_path_is_kept_as_written(self):
        for value in ("/panel", "/dev/front~panel_1.0-a"):
            with self.subTest(value=value):
                self.assertEqual(URL_PREFIX(value), value)

    def test_a_prefix_that_is_not_absolute_is_refused(self):
        for value in ("panel", "", "panel/"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "must start with '/'"),
            ):
                URL_PREFIX(value)

    def test_the_server_root_and_a_trailing_slash_are_refused(self):
        # Both would swallow the sub-paths the component serves under the prefix.
        for value in ("/", "/panel/"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "must not be '/' or end with '/'"),
            ):
                URL_PREFIX(value)

    def test_an_empty_or_dot_segment_is_refused(self):
        # A browser resolves these away, so the prefix it sends is not the one configured.
        for value in ("/a//b", "/a/../panel", "/a/./b"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "must not contain empty or dot path segments"
                ),
            ):
                URL_PREFIX(value)

    def test_what_a_browser_rewrites_or_encodes_is_refused(self):
        for value in (
            "/pa nel",
            "/panel?x",
            "/panel#top",
            "/pa+nel",
            "/%70anel",
            "/pa\\nel",
            "/pa\tnel",
            "/pa\x01nel",
            "/panel\n",
            "/панель",
        ):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "must be a URL path a browser sends unchanged"
                ),
            ):
                URL_PREFIX(value)

    def test_anything_but_a_string_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Must be string"):
            URL_PREFIX(42)


class Schema(unittest.TestCase):
    def test_the_url_prefix_defaults_to_the_panel_path(self):
        config = virtual_display.CONFIG_SCHEMA({})
        self.assertEqual(config[virtual_display.CONF_URL_PREFIX], "/panel")

    def test_the_dimensions_default_to_the_common_oled(self):
        config = virtual_display.CONFIG_SCHEMA({})
        self.assertEqual((config[CONF_WIDTH], config[CONF_HEIGHT]), (128, 64))

    def test_the_hold_time_defaults_to_120ms(self):
        config = virtual_display.CONFIG_SCHEMA({})
        self.assertEqual(
            config[virtual_display.CONF_HOLD_TIME], cv.TimePeriod(milliseconds=120)
        )

    def test_a_key_is_kept_under_its_name(self):
        config = virtual_display.CONFIG_SCHEMA({"keys": {"up": "btn_up"}})
        self.assertEqual(list(config[virtual_display.CONF_KEYS]), ["up"])

    def test_a_bad_key_name_is_refused_by_name(self):
        # The name is checked over the whole mapping, so the message reaches the user
        # instead of voluptuous's "extra keys not allowed".
        for value in ("up down", "up\n", 42):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "Key name|Must be string"),
            ):
                virtual_display.CONFIG_SCHEMA({"keys": {value: "btn_up"}})


if __name__ == "__main__":
    unittest.main()
