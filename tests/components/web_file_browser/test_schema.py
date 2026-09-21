"""The component's YAML schema: what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import web_file_browser as browser  # noqa: E402


class UrlPrefix(unittest.TestCase):
    def test_a_path_is_kept(self):
        self.assertEqual(browser.url_prefix("/files"), "/files")

    def test_a_nested_path_is_kept(self):
        self.assertEqual(browser.url_prefix("/api/v1/files"), "/api/v1/files")

    def test_the_slashes_are_normalized(self):
        for value in ("files", "files/", "/files/", "//files//"):
            with self.subTest(value=value):
                self.assertEqual(browser.url_prefix(value), "/files")

    def test_the_server_root_is_refused(self):
        for value in ("", "/", "///"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "url_prefix must name a path"),
            ):
                browser.url_prefix(value)

    def test_empty_and_dot_segments_are_refused(self):
        # A browser resolves these before sending, so the handler would never see the prefix.
        for value in ("/files//api", "/files/./api", "/files/../api", "/.", "/.."):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "empty or dot path segments"),
            ):
                browser.url_prefix(value)

    def test_what_a_browser_would_rewrite_is_refused(self):
        for value in (
            "/fi les",
            "/files?x",
            "/files#top",
            "/fi\tles",
            "/fi\\les",
            "/%2e%2e/files",
            "/files/a+b",
            "/файлы",
        ):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "url_prefix must be a URL path"),
            ):
                browser.url_prefix(value)

    def test_anything_but_a_string_is_refused(self):
        with self.assertRaises(cv.Invalid):
            browser.url_prefix(42)

    def test_the_default_is_the_files_path(self):
        config = browser.CONFIG_SCHEMA({"storage_id": "test_storage"})
        self.assertEqual(config[browser.CONF_URL_PREFIX], "/files")


if __name__ == "__main__":
    unittest.main()
