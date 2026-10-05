"""The component's YAML schema: what it accepts, what it refuses, and with which message."""

import re
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
                self.assertRaisesRegex(
                    cv.Invalid,
                    re.escape(
                        "url_prefix must be a URL path a browser sends unchanged: "
                        "letters, digits, '-', '.', '_' and '~'"
                    ),
                ),
            ):
                editor.url_prefix(value)

    def test_every_character_a_browser_leaves_alone_is_kept(self):
        self.assertEqual(editor.url_prefix("/A-z_0.9~x"), "/A-z_0.9~x")

    def test_a_path_web_server_answers_is_refused(self):
        # The editor claims everything below its prefix; web_server answers these itself.
        for first in sorted(editor.WEB_SERVER_PATHS):
            for value in (f"/{first}", f"/{first}/editor"):
                with (
                    self.subTest(value=value),
                    self.assertRaisesRegex(
                        cv.Invalid,
                        re.escape(
                            f"url_prefix must not start with '/{first}': "
                            "web_server answers that path"
                        ),
                    ),
                ):
                    editor.url_prefix(value)

    def test_every_path_web_server_answers_is_listed(self):
        # Its entity domains, its event stream and the files of its own page.
        for first in (
            "climate",
            "switch",
            "sensor",
            "events",
            "update",
            "0.css",
            "0.js",
        ):
            with self.subTest(first=first):
                self.assertIn(first, editor.WEB_SERVER_PATHS)

    def test_a_web_server_word_further_down_is_fine(self):
        for value in ("/ui/climate", "/climates", "/my-switch", "/editor/events"):
            with self.subTest(value=value):
                self.assertEqual(editor.url_prefix(value), value)

    def test_the_dashboards_api_is_refused(self):
        for value in ("/api", "api/", "/api/device", "/api/device/climate"):
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
        for value in (42, None, True, ["/editor"]):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "Must be string"),
            ):
                editor.url_prefix(value)

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
        # A sibling that only shares the first characters is apart too.
        self.validate("/files-2", {"web_file_browser": {"url_prefix": "/files"}})

    def test_a_component_without_a_prefix_is_skipped(self):
        # web_automation_editor is absent or not yet validated: nothing to compare with.
        self.validate(
            "/climate-editor",
            {"web_file_browser": None, "web_automation_editor": {"storage_id": "x"}},
        )

    def test_the_config_passes_through_unchanged(self):
        config = self.validate("/climate-editor", {})
        self.assertEqual(config, {editor.CONF_URL_PREFIX: "/climate-editor"})

    def test_a_prefix_on_or_below_another_handlers_is_refused(self):
        for ours, component, theirs in (
            ("/files", "web_file_browser", "/files"),
            ("/files/climate", "web_file_browser", "/files"),
            ("/edit", "web_automation_editor", "/edit/rules"),
            ("/edit/rules/climate", "web_automation_editor", "/edit/rules"),
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
