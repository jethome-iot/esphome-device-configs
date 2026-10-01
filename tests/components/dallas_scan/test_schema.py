"""The component's YAML schema: what it accepts, what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.const import KEY_CORE, KEY_TARGET_PLATFORM, PLATFORM_HOST
from esphome.core import CORE, ID

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import dallas_scan  # noqa: E402


def setUpModule():
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST


def validate(config):
    return dallas_scan.CONFIG_SCHEMA(config)


def final_validate(config, full_config):
    token = fv.full_config.set(full_config)
    try:
        return dallas_scan.FINAL_VALIDATE_SCHEMA(validate(config))
    finally:
        fv.full_config.reset(token)


def sensor_entry(name, **options):
    """A validated sensor: entry, as the full config holds it."""
    return {"id": ID(name, is_declaration=True), **options}


class Defaults(unittest.TestCase):
    def test_an_empty_config_is_enough(self):
        config = validate({})
        self.assertEqual(config["max_sensors"], 8)
        self.assertEqual(config["name_prefix"], "Temp")
        self.assertEqual(config["resolution"], 12)
        self.assertEqual(config["sensors"], [])

    def test_the_bus_is_found_without_being_named(self):
        bus = validate({})["one_wire_id"]
        self.assertEqual(bus.type, "one_wire::OneWireBus")
        self.assertFalse(bus.is_declaration)


class Ranges(unittest.TestCase):
    def test_max_sensors_is_one_to_sixty_four(self):
        for value in (1, 64):
            with self.subTest(value=value):
                self.assertEqual(validate({"max_sensors": value})["max_sensors"], value)
        for value in (0, 65):
            with self.subTest(value=value), self.assertRaises(cv.Invalid):
                validate({"max_sensors": value})

    def test_resolution_is_nine_to_twelve_bits(self):
        for value in (8, 13):
            with self.subTest(value=value), self.assertRaises(cv.Invalid):
                validate({"resolution": value})


class Sensors(unittest.TestCase):
    def test_listed_sensors_keep_their_order(self):
        config = validate({"sensors": ["boiler", "floor"]})
        self.assertEqual([s.id for s in config["sensors"]], ["boiler", "floor"])

    def test_more_listed_sensors_than_slots_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "3 sensors listed, max_sensors is 2"):
            validate({"max_sensors": 2, "sensors": ["a", "b", "c"]})

    def test_a_sensor_listed_twice_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "A sensor is listed twice"):
            validate({"sensors": ["boiler", "boiler"]})


class OneWireSensors(unittest.TestCase):
    def test_a_listed_one_wire_sensor_needs_an_address(self):
        full_config = {"sensor": [sensor_entry("boiler", one_wire_id="bus", index=0)]}
        with self.assertRaisesRegex(
            cv.Invalid,
            "boiler is a 1-Wire sensor: give it an address instead of an index "
            "to list it in sensors",
        ):
            final_validate({"sensors": ["boiler"]}, full_config)

    def test_with_an_address_or_off_the_bus_it_passes(self):
        full_config = {
            "sensor": [
                sensor_entry("boiler", one_wire_id="bus", address=0x8A0122791699DD28),
                sensor_entry("outdoor", platform="template"),
            ]
        }
        final_validate({"sensors": ["boiler", "outdoor"]}, full_config)


class Storage(unittest.TestCase):
    def test_preferences_are_the_default(self):
        config = validate({})
        self.assertEqual(config["storage"], "nvs")
        self.assertNotIn("config_json_id", config)

    def test_a_file_is_the_other_choice(self):
        self.assertEqual(validate({"storage": "File"})["storage"], "file")

    def test_an_unknown_storage_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Unknown value 'flash'"):
            validate({"storage": "flash"})

    def test_a_keeper_for_preferences_is_refused(self):
        for config in (
            {"config_json_id": "settings"},
            {"storage": "nvs", "config_json_id": "settings"},
        ):
            with (
                self.subTest(config=config),
                self.assertRaisesRegex(
                    cv.Invalid, "config_json_id only applies to storage: file"
                ),
            ):
                validate(config)

    def test_the_keeper_may_be_named(self):
        config = validate({"storage": "file", "config_json_id": "settings"})
        self.assertEqual(config["config_json_id"].id, "settings")
        self.assertEqual(config["config_json_id"].type, "config_json::ConfigJsonKeeper")
        self.assertNotIn("config_json_id", validate({"storage": "file"}))


class StorageFinalValidate(unittest.TestCase):
    def test_a_file_needs_a_config_json_section(self):
        with self.assertRaisesRegex(
            cv.Invalid, "storage: file needs a config_json: section"
        ):
            final_validate({"storage": "file"}, {"dallas_scan": {}})

    def test_a_file_with_a_config_json_section_passes(self):
        final_validate({"storage": "file"}, {"config_json": {"id": ID("settings")}})

    def test_a_file_with_a_named_keeper_passes(self):
        # The id pass runs before this one and refuses an id that is not there.
        final_validate({"storage": "file", "config_json_id": "settings"}, {})

    def test_preferences_need_no_config_json_section(self):
        final_validate({}, {})


if __name__ == "__main__":
    unittest.main()
