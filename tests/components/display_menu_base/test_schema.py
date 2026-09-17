"""The YAML surface the two components add over upstream's: the options, the back action,
the item weights and the submenu that may stay empty."""

import unittest
from pathlib import Path

from esphome import automation, loader
from esphome.config import resolve_extend_remove
from esphome.config_helpers import Extend, merge_config
import esphome.config_validation as cv

loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import display_menu_base, graphical_display_menu  # noqa: E402

ITEMS = [{"type": "label", "text": "Row"}]


def menu(**options):
    return display_menu_base.DISPLAY_MENU_BASE_SCHEMA({"items": ITEMS, **options})


def label(text, **options):
    return {"type": "label", "text": text, **options}


def order(*rows):
    """The texts of a root's items, in the order validation put them in."""
    return [item["text"] for item in menu(items=list(rows))["items"]]


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


class Weight(unittest.TestCase):
    def test_items_are_sorted_by_it(self):
        self.assertEqual(
            order(label("C", weight=3), label("A", weight=1), label("B", weight=2)),
            ["A", "B", "C"],
        )

    def test_every_item_type_takes_one(self):
        for item in (
            {"type": "label"},
            {"type": "back"},
            {"type": "menu"},
            {"type": "select", "select": "a_select"},
            {"type": "number", "number": "a_number"},
            {"type": "switch", "switch": "a_switch"},
            {"type": "command"},
            {"type": "custom"},
        ):
            with self.subTest(type=item["type"]):
                config = menu(items=[{**item, "text": "Row", "weight": 7}])
                self.assertEqual(config["items"][0]["weight"], 7)

    def test_it_sorts_items_of_different_types_together(self):
        config = menu(
            items=[
                {"type": "command", "text": "Command", "weight": 2},
                {"type": "menu", "text": "Menu", "weight": 1},
            ]
        )
        self.assertEqual(
            [item["text"] for item in config["items"]], ["Menu", "Command"]
        )

    def test_equal_weights_keep_their_declaration_order(self):
        self.assertEqual(
            order(label("B", weight=2), label("A", weight=2), label("C", weight=2)),
            ["B", "A", "C"],
        )

    def test_absent_weights_keep_their_declaration_order(self):
        self.assertEqual(order(label("B"), label("A"), label("C")), ["B", "A", "C"])

    def test_an_item_without_one_sorts_as_zero(self):
        self.assertEqual(
            order(label("Late", weight=1), label("Middle"), label("Early", weight=-1)),
            ["Early", "Middle", "Late"],
        )

    def test_a_negative_weight_sorts_first(self):
        self.assertEqual(
            order(label("A"), label("B"), label("Top", weight=-1)),
            ["Top", "A", "B"],
        )

    def test_it_has_no_default(self):
        self.assertNotIn("weight", menu()["items"][0])

    def test_a_nested_menu_sorts_independently_of_its_parent(self):
        config = menu(
            items=[
                label("Last", weight=5),
                {
                    "type": "menu",
                    "text": "Sub",
                    "weight": 1,
                    "items": [label("Inner", weight=2), label("First", weight=-1)],
                },
            ]
        )

        self.assertEqual([item["text"] for item in config["items"]], ["Sub", "Last"])
        self.assertEqual(
            [item["text"] for item in config["items"][0]["items"]], ["First", "Inner"]
        )

    def test_a_non_integer_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Expected integer"):
            order(label("A", weight="heavy"))

    def test_a_fractional_weight_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "only accepts integers"):
            order(label("A", weight=1.5))


class EmptyMenu(unittest.TestCase):
    def test_a_submenu_takes_an_empty_items_list(self):
        config = menu(items=[{"type": "menu", "text": "Sub", "items": []}])

        self.assertEqual(config["items"][0]["items"], [])

    def test_a_submenu_takes_no_items_key_at_all(self):
        config = menu(items=[{"type": "menu", "text": "Sub"}])

        self.assertNotIn("items", config["items"][0])

    def test_a_submenu_of_a_submenu_may_be_empty_too(self):
        config = menu(
            items=[
                {
                    "type": "menu",
                    "text": "Sub",
                    "items": [{"type": "menu", "text": "Deeper", "items": []}],
                }
            ]
        )

        self.assertEqual(config["items"][0]["items"][0]["items"], [])

    def test_the_root_refuses_an_empty_items_list(self):
        with self.assertRaisesRegex(cv.Invalid, "at least 1"):
            menu(items=[])

    def test_the_root_refuses_a_missing_items_key(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            display_menu_base.DISPLAY_MENU_BASE_SCHEMA({})


def info_menu(*rows):
    """A package declaring the Info submenu, as menu.yaml does."""
    return {"items": [{"type": "menu", "id": "info_submenu", "items": list(rows)}]}


def extends_info(*rows):
    """A package appending to it by id, as menu-items-network.yaml and menu-serial.yaml do."""
    return {"items": [{"id": Extend("info_submenu"), "items": list(rows)}]}


class WeightAcrossPackages(unittest.TestCase):
    """A weight has to survive package merge, which is the only reason menu-serial.yaml works.

    ESPHome concatenates the two `items:` lists while merging packages and resolves `!extend`
    before it validates the component, so the sort sees one merged list. Both are upstream
    internals a version bump could move, and if either did, the weighted rows would quietly
    drift up the menu instead of failing a build.
    """

    def info_rows(self, *packages):
        # Each package is rebuilt per merge: resolving an !extend deletes the marker off it.
        merged = {}
        for build in packages:
            merged = merge_config(merged, build())
        resolve_extend_remove(merged)
        config = display_menu_base.DISPLAY_MENU_BASE_SCHEMA(merged)
        return [row["text"] for row in config["items"][0]["items"]]

    def test_a_weighted_row_stays_last_whatever_the_package_order(self):
        base = lambda: info_menu()  # noqa: E731
        network = lambda: extends_info(label("IP"), label("MAC"))  # noqa: E731
        serial = lambda: extends_info(label("Serial", weight=10))  # noqa: E731

        for name, order_of in (
            ("network first", (network, serial)),
            ("serial first", (serial, network)),
        ):
            with self.subTest(packages=name):
                self.assertEqual(
                    self.info_rows(base, *order_of), ["IP", "MAC", "Serial"]
                )

    def test_without_a_weight_the_package_order_decides(self):
        base = lambda: info_menu()  # noqa: E731
        network = lambda: extends_info(label("IP"), label("MAC"))  # noqa: E731
        serial = lambda: extends_info(label("Serial"))  # noqa: E731

        self.assertEqual(self.info_rows(base, serial, network), ["Serial", "IP", "MAC"])


if __name__ == "__main__":
    unittest.main()
