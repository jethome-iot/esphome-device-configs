"""The component's YAML schema: what it accepts, what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import web_climate_editor as editor  # noqa: E402


class UrlPrefix(unittest.TestCase):
    def test_a_path_is_kept(self):
        for value in (
            "/thermostats",
            "/ui/climate",
            "/api/devices",
            "/api/thermostats",
        ):
            with self.subTest(value=value):
                self.assertEqual(editor.url_prefix(value), value)

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

    def test_dot_segments_are_refused(self):
        for value in ("/a/../b", "/./a", "/a//b"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "url_prefix must not contain empty or dot path segments"
                ),
            ):
                editor.url_prefix(value)

    def test_what_is_not_a_path_is_refused(self):
        for value in (
            "/ed itor",
            "/editor?x",
            "/editor#top",
            "/edi\\tor",
            "/%2e%2e/editor",
            "/редактор",
        ):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "url_prefix must be a URL path"),
            ):
                editor.url_prefix(value)

    def test_a_path_web_server_answers_is_refused(self):
        # The editor claims everything below its prefix; web_server answers these itself.
        for value, first in (
            ("/climate", "climate"),
            ("/switch/editor", "switch"),
            ("/sensor", "sensor"),
            ("/events", "events"),
            ("/update", "update"),
        ):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid,
                    f"url_prefix must not start with '/{first}': web_server answers that path",
                ),
            ):
                editor.url_prefix(value)

    def test_the_dashboards_api_is_refused(self):
        for value in ("/api", "/api/device", "/api/device/climate"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid,
                    "url_prefix must stay clear of /api/device, "
                    "which the device dashboard serves",
                ),
            ):
                editor.url_prefix(value)

    def test_anything_but_a_string_is_refused(self):
        with self.assertRaises(cv.Invalid):
            editor.url_prefix(42)

    def test_the_default_is_the_editor_path(self):
        config = editor.CONFIG_SCHEMA({})
        self.assertEqual(config[editor.CONF_URL_PREFIX], "/climate-editor")


class Wiring(unittest.TestCase):
    def test_the_hub_is_found_without_being_named(self):
        config = editor.CONFIG_SCHEMA({})
        hub = config[editor.CONF_CLIMATE_HUB_ID]
        self.assertEqual(hub.type, "climate_hub::ClimateHub")
        self.assertFalse(hub.is_declaration)

    def test_it_needs_the_server_and_the_hub(self):
        self.assertEqual(editor.DEPENDENCIES, ["web_server_base", "climate_hub"])
        self.assertEqual(editor.AUTO_LOAD, ["web_origin_guard", "loop_job"])


class FinalValidate(unittest.TestCase):
    def validate(self, prefix, full_config):
        token = fv.full_config.set(full_config)
        try:
            return editor.FINAL_VALIDATE_SCHEMA({editor.CONF_URL_PREFIX: prefix})
        finally:
            fv.full_config.reset(token)

    def test_prefixes_apart_pass(self):
        self.validate(
            "/climate-editor",
            {
                "web_file_browser": {"url_prefix": "/files"},
                "web_automation_editor": {"url_prefix": "/automation-editor"},
            },
        )
        self.validate("/climate-editor", {})

    def test_a_prefix_on_or_below_another_handlers_is_refused(self):
        for ours, component, theirs in (
            ("/files", "web_file_browser", "/files"),
            ("/files/climate", "web_file_browser", "/files"),
            ("/edit", "web_automation_editor", "/edit/rules"),
        ):
            with (
                self.subTest(ours=ours, component=component),
                self.assertRaisesRegex(
                    cv.Invalid,
                    f"url_prefix '{ours}' overlaps {component}'s '{theirs}': "
                    "both claim what lies below it",
                ),
            ):
                self.validate(ours, {component: {"url_prefix": theirs}})


if __name__ == "__main__":
    unittest.main()
