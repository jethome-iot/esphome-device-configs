"""The component's YAML schema: there is nothing to configure."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import case_fold  # noqa: E402
from esphome.components import automations, climate_hub, schedules  # noqa: E402


class Schema(unittest.TestCase):
    def test_an_empty_block_is_accepted(self):
        self.assertEqual(case_fold.CONFIG_SCHEMA({}), {})

    def test_any_option_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"):
            case_fold.CONFIG_SCHEMA({"scripts": ["latin"]})

    def test_every_engine_that_compares_names_pulls_it_in(self):
        for engine in (automations, climate_hub, schedules):
            with self.subTest(engine=engine.__name__):
                self.assertIn("case_fold", engine.AUTO_LOAD)


if __name__ == "__main__":
    unittest.main()
