"""The component's YAML schema: what it accepts, what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_PLATFORM,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
)
from esphome.core import CORE

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import virtual_inputs  # noqa: E402


def setUpModule():
    # only_on() reads the target platform; the suite builds for host.
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST


class Defaults(unittest.TestCase):
    def test_nothing_is_required(self):
        config = virtual_inputs.CONFIG_SCHEMA({})
        self.assertEqual(config[virtual_inputs.CONF_MAX_INPUTS], 16)
        self.assertNotIn("web_server", config)
        self.assertIn(virtual_inputs.CONF_CONFIG_JSON_ID, config)

    def test_the_slots_live_in_a_config_json_folder(self):
        self.assertEqual(virtual_inputs.DEPENDENCIES, ["config_json"])
        self.assertEqual(virtual_inputs.AUTO_LOAD, ["binary_sensor", "json"])


class MaxInputs(unittest.TestCase):
    def test_the_range_is_one_to_thirty_two(self):
        for value in (1, 16, 32):
            config = virtual_inputs.CONFIG_SCHEMA({"max_inputs": value})
            self.assertEqual(config[virtual_inputs.CONF_MAX_INPUTS], value)

    def test_outside_the_range_is_refused(self):
        for value, message in (
            (0, "at least 1"),
            (33, "at most 32"),
            (-4, "at least 1"),
        ):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, message),
            ):
                virtual_inputs.CONFIG_SCHEMA({"max_inputs": value})

    def test_a_number_that_is_not_a_whole_one_is_refused(self):
        for value in ("many", 2.5):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "integer"),
            ):
                virtual_inputs.CONFIG_SCHEMA({"max_inputs": value})


class SlotNames(unittest.TestCase):
    def test_slot_n_is_virtual_input_n(self):
        self.assertEqual(virtual_inputs.slot_object_id(1), "virtual_input_1")
        self.assertEqual(virtual_inputs.slot_object_id(32), "virtual_input_32")


class Platform(unittest.TestCase):
    def test_other_platforms_are_refused(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP8266
        try:
            with self.assertRaisesRegex(
                cv.Invalid, r"This feature is only available on \[.*'esp32'.*'host'.*\]"
            ):
                virtual_inputs.CONFIG_SCHEMA({})
        finally:
            CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_HOST


class WebServerSorting(unittest.TestCase):
    def setUp(self):
        self.had_web_server = "web_server" in CORE.loaded_integrations

    def tearDown(self):
        if self.had_web_server:
            CORE.loaded_integrations.add("web_server")
        else:
            CORE.loaded_integrations.discard("web_server")

    def test_a_sorting_group_and_weight_are_taken_like_any_entitys(self):
        CORE.loaded_integrations.add("web_server")
        config = virtual_inputs.CONFIG_SCHEMA(
            {"web_server": {"sorting_group_id": "group_inputs", "sorting_weight": 7}}
        )
        sorting = config["web_server"]
        self.assertEqual(str(sorting["sorting_group_id"]), "group_inputs")
        self.assertEqual(sorting["sorting_weight"], 7.0)

    def test_sorting_without_a_web_server_is_refused(self):
        CORE.loaded_integrations.discard("web_server")
        with self.assertRaisesRegex(
            cv.Invalid, "This option requires component web_server"
        ):
            virtual_inputs.CONFIG_SCHEMA(
                {"web_server": {"sorting_group_id": "group_inputs"}}
            )


class FinalValidate(unittest.TestCase):
    def validate(self, full_config, max_inputs=16):
        token = fv.full_config.set(full_config)
        try:
            return virtual_inputs.FINAL_VALIDATE_SCHEMA({"max_inputs": max_inputs})
        finally:
            fv.full_config.reset(token)

    def test_the_jxd_inputs_pass(self):
        self.validate(
            {
                "web_server": {"include_internal": False},
                "binary_sensor": [{"name": f"Input {n}"} for n in range(1, 7)]
                + [{"name": "FN", "internal": True}, {"id": "unnamed"}],
            }
        )
        self.validate({})

    def test_a_web_server_listing_internal_entities_is_refused(self):
        # It would list every free slot.
        with self.assertRaisesRegex(
            cv.Invalid,
            "virtual_inputs keeps its free slots internal; "
            "web_server include_internal: true would list them",
        ):
            self.validate({"web_server": {"include_internal": True}})

    def test_a_binary_sensor_with_a_slots_object_id_is_refused(self):
        # The slots are registered at run time, where nothing checks for a twin.
        for name, slot in (
            ("Virtual input 3", 3),
            ("virtual input 16", 16),
            ("Virtual_Input_1", 1),
        ):
            with (
                self.subTest(name=name),
                self.assertRaisesRegex(
                    cv.Invalid,
                    f"binary_sensor '{name}' has the object id of Virtual input {slot}; "
                    "give it another name",
                ),
            ):
                self.validate({"binary_sensor": [{"name": "Input 1"}, {"name": name}]})

    def test_a_slot_past_max_inputs_is_no_twin(self):
        self.validate({"binary_sensor": [{"name": "Virtual input 5"}]}, max_inputs=4)


if __name__ == "__main__":
    unittest.main()
