"""The YAML surface the two components add over upstream's: the options and the back action."""

import unittest
from pathlib import Path

from esphome import automation, loader
import esphome.config_validation as cv

loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import display_menu_base, graphical_display_menu  # noqa: E402

ITEMS = [{"type": "label", "text": "Row"}]


def menu(**options):
    return display_menu_base.DISPLAY_MENU_BASE_SCHEMA({"items": ITEMS, **options})


def graphical(**options):
    return graphical_display_menu.CONFIG_SCHEMA(
        {"font": "menu_font", "items": ITEMS, **options}
    )


class RightForMenuEnter(unittest.TestCase):
    def test_it_is_on_by_default(self):
        self.assertTrue(menu()["right_for_menu_enter"])

    def test_it_can_be_turned_off(self):
        self.assertFalse(menu(right_for_menu_enter=False)["right_for_menu_enter"])

    def test_a_non_boolean_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Expected boolean"):
            menu(right_for_menu_enter="sometimes")


class FillRow(unittest.TestCase):
    def test_it_is_off_by_default(self):
        self.assertFalse(graphical()["fill_row"])

    def test_it_can_be_turned_on(self):
        self.assertTrue(graphical(fill_row=True)["fill_row"])

    def test_a_non_boolean_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Expected boolean"):
            graphical(fill_row="wide")


class BackAction(unittest.TestCase):
    def test_it_is_registered(self):
        self.assertIn("display_menu.back", automation.ACTION_REGISTRY)

    def test_it_takes_the_menu_id_on_its_own(self):
        config = automation.ACTION_REGISTRY["display_menu.back"].schema("display_menu")
        self.assertEqual(config["id"].id, "display_menu")


if __name__ == "__main__":
    unittest.main()
