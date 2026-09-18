"""The component's YAML schema: what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import web_automation_editor as editor  # noqa: E402


class UrlPrefix(unittest.TestCase):
    def test_a_path_is_kept(self):
        self.assertEqual(editor.url_prefix("/editor"), "/editor")

    def test_the_slashes_are_normalized(self):
        for value in ("editor", "editor/", "/editor/", "//editor//"):
            with self.subTest(value=value):
                self.assertEqual(editor.url_prefix(value), "/editor")

    def test_the_server_root_is_refused(self):
        for value in ("", "/", "///"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "url_prefix must name a path"),
            ):
                editor.url_prefix(value)

    def test_what_is_not_a_path_is_refused(self):
        for value in (
            "/ed itor",
            "/editor?x",
            "/editor#top",
            "/edi\ttor",
            "/edi\\tor",
            "/%2e%2e/editor",
            "/\u0440\u0435\u0434\u0430\u043a\u0442\u043e\u0440",
        ):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "url_prefix must be a URL path"),
            ):
                editor.url_prefix(value)

    def test_anything_but_a_string_is_refused(self):
        with self.assertRaises(cv.Invalid):
            editor.url_prefix(42)

    def test_the_default_is_the_editor_path(self):
        config = editor.CONFIG_SCHEMA({})
        self.assertEqual(config[editor.CONF_URL_PREFIX], "/automation-editor")


if __name__ == "__main__":
    unittest.main()
