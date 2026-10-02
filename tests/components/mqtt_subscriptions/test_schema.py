"""The component's YAML schema, the folder it may not share, and what codegen emits."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_FRAMEWORK,
    KEY_TARGET_PLATFORM,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
)
from esphome.core import CORE, ID
from esphome.cpp_generator import MockObj

ROOT = Path(__file__).resolve().parents[3]

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(ROOT / "components")

from esphome.components import mqtt_subscriptions as subs  # noqa: E402


def on(platform):
    CORE.reset()
    CORE.name = "jxd-r6-e1eth-lcd"
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = platform
    CORE.data[KEY_CORE][KEY_TARGET_FRAMEWORK] = "esp-idf"


def validate(**options):
    return subs.CONFIG_SCHEMA({"storage": "user_storage", **options})


class Schema(unittest.TestCase):
    def setUp(self):
        on(PLATFORM_HOST)

    def test_the_defaults(self):
        config = validate()
        self.assertEqual(config["max_slots"], 8)
        self.assertEqual(config["folder_path"], "mqtt")
        self.assertEqual(config["units"], subs.DEFAULT_UNITS)
        self.assertEqual(config["storage"].id, "user_storage")
        self.assertIn("mqtt_config_id", config)

    def test_the_storage_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "storage"):
            subs.CONFIG_SCHEMA({})

    def test_one_to_sixteen_slots(self):
        self.assertEqual(validate(max_slots=1)["max_slots"], 1)
        self.assertEqual(validate(max_slots=16)["max_slots"], 16)
        for bad in (0, 17):
            with self.subTest(max_slots=bad), self.assertRaises(cv.Invalid):
                validate(max_slots=bad)

    def test_the_folder_is_one_name(self):
        self.assertEqual(validate(folder_path="slots")["folder_path"], "slots")
        for bad in ("", "a/b", "a\\b", ".", ".."):
            with self.subTest(folder_path=bad):
                with self.assertRaises(cv.Invalid) as refused:
                    validate(folder_path=bad)
                self.assertEqual(
                    refused.exception.msg, "folder_path must be a single folder name"
                )

    def test_the_default_units_pass_their_own_rules(self):
        self.assertEqual(subs.unit_list(subs.DEFAULT_UNITS), subs.DEFAULT_UNITS)

    def assert_units_refused(self, units, message):
        with self.assertRaises(cv.Invalid) as refused:
            validate(units=units)
        self.assertEqual(refused.exception.msg, message)

    def test_a_unit_listed_twice_is_refused(self):
        self.assert_units_refused(["°C", "%", "°C"], "unit '°C' is listed twice")

    def test_a_unit_is_one_to_sixteen_bytes(self):
        self.assert_units_refused(["°C", ""], "unit '' must be 1 to 16 bytes")
        # Bytes: nine '°' are eighteen.
        self.assert_units_refused(["°" * 9], f"unit '{'°' * 9}' must be 1 to 16 bytes")
        self.assertEqual(validate(units=["x" * 16])["units"], ["x" * 16])

    def test_at_most_thirty_two_units(self):
        self.assert_units_refused([f"u{i}" for i in range(33)], "at most 32 units")
        self.assertEqual(len(validate(units=[f"u{i}" for i in range(32)])["units"]), 32)

    def test_a_single_unit_is_a_list(self):
        self.assertEqual(validate(units="°C")["units"], ["°C"])

    def test_the_sorting_block_takes_a_group_and_a_weight(self):
        CORE.loaded_integrations.add("web_server")
        config = validate(
            web_server={"sorting_group_id": "group_mqtt", "sorting_weight": 1}
        )
        self.assertEqual(config["web_server"]["sorting_group_id"].id, "group_mqtt")
        self.assertEqual(config["web_server"]["sorting_weight"], 1)

    def test_an_unknown_option_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "qos"):
            validate(qos=1)

    def test_esp32_is_accepted(self):
        on(PLATFORM_ESP32)
        validate()

    def test_another_platform_is_refused(self):
        on(PLATFORM_ESP8266)
        with self.assertRaisesRegex(cv.Invalid, "only available on"):
            validate()


class SharedFolder(unittest.TestCase):
    def setUp(self):
        on(PLATFORM_HOST)

    def final_validate(self, config, full_config):
        token = fv.full_config.set(full_config)
        try:
            return subs.FINAL_VALIDATE_SCHEMA(config)
        finally:
            fv.full_config.reset(token)

    def storage(self, name="user_storage"):
        return ID(name)

    def test_another_components_folder_on_the_same_storage_is_refused(self):
        for domain, key, folder in (
            ("automations", "folder_path", "automations"),
            ("crash_report", "report_dir", "crash"),
            ("config_json", "config_dir", "config"),
        ):
            with self.subTest(domain=domain):
                config = {"storage": self.storage(), "folder_path": folder}
                full = {domain: {"storage": self.storage(), key: folder}}
                with self.assertRaises(cv.Invalid) as refused:
                    self.final_validate(config, full)
                self.assertEqual(
                    refused.exception.msg,
                    f"folder_path '{folder}' is {domain}'s folder on the same storage: "
                    "pick another",
                )
                self.assertEqual(refused.exception.path, ["folder_path"])

    def test_the_folder_dallas_scan_keeps_its_slot_table_in_is_refused(self):
        # With storage: file the table is <config_dir>/<key>.json, config_json's folder.
        config = {"storage": self.storage(), "folder_path": "config"}
        full = {
            "config_json": {"storage": self.storage(), "config_dir": "config"},
            "dallas_scan": {
                "storage": "file",
                "name_prefix": "Temp",
                "max_sensors": 16,
            },
        }
        with self.assertRaises(cv.Invalid) as refused:
            self.final_validate(config, full)
        self.assertIn("config_json's folder", refused.exception.msg)

    def test_another_storage_or_another_folder_passes(self):
        config = {"storage": self.storage(), "folder_path": "mqtt"}
        self.final_validate(
            config,
            {
                "automations": {"storage": self.storage(), "folder_path": "rules"},
                "crash_report": {"storage": self.storage("sd"), "report_dir": "mqtt"},
            },
        )
        self.final_validate(config, {})


class Codegen(unittest.TestCase):
    """to_code over a config whose ids are already registered, as codegen sees them."""

    def emit(self, full_config, **options):
        on(PLATFORM_HOST)
        if "web_server" in options:
            CORE.loaded_integrations.add("web_server")
        config = validate(**options)
        config["id"] = ID("mqtt_subs", is_declaration=True, type=subs.MqttSubscriptions)
        config["mqtt_config_id"] = ID("mqtt_settings")
        CORE.component_ids.add("mqtt_subs")
        CORE.register_variable(config["mqtt_config_id"], MockObj("mqtt_settings"))
        CORE.register_variable(config["storage"], MockObj("user_storage"))
        if sorting := config.get("web_server"):
            sorting["web_server_id"] = ID("web_server_base")
            CORE.register_variable(sorting["web_server_id"], MockObj("web_server_base"))
        CORE.config = full_config
        # As codegen runs it, with the jobs it queues (the unit string table) run after it.
        CORE.add_job(subs.to_code, config)
        CORE.flush_tasks()
        return [str(statement) for statement in CORE.main_statements]

    def test_each_domain_gets_a_place_per_slot(self):
        self.emit({}, max_slots=5)
        for domain in ("sensor", "binary_sensor", "text_sensor"):
            self.assertEqual(CORE.platform_counts[domain], 5)

    def test_the_setters_and_the_units(self):
        lines = self.emit({}, max_slots=4, units=["°C", "%"], folder_path="slots")
        self.assertIn("mqtt_subs->set_config(mqtt_settings);", lines)
        self.assertIn("mqtt_subs->set_storage(user_storage);", lines)
        self.assertIn('mqtt_subs->set_folder_path("slots");', lines)
        self.assertIn("mqtt_subs->set_max_slots(4);", lines)
        self.assertIn('mqtt_subs->add_unit("\\302\\260C", 1);', lines)
        self.assertIn('mqtt_subs->add_unit("%", 2);', lines)
        self.assertIn("USE_MQTT_SUBSCRIPTIONS", {d.name for d in CORE.defines})

    def test_the_names_probes_take_are_reserved(self):
        lines = self.emit({"dallas_scan": {"name_prefix": "Temp", "max_sensors": 16}})
        self.assertIn('mqtt_subs->reserve_sensor_names("Temp", 16);', lines)
        self.assertEqual(
            subs.probe_names({"dallas_scan": {"name_prefix": "T", "max_sensors": 2}}),
            ("T", 2),
        )

    def test_without_probes_nothing_is_reserved(self):
        lines = self.emit({})
        self.assertFalse([line for line in lines if "reserve_sensor_names" in line])
        self.assertIsNone(subs.probe_names({}))

    def test_the_sorting_group(self):
        lines = self.emit(
            {}, web_server={"sorting_group_id": "group_mqtt", "sorting_weight": 1}
        )
        # The hash web_server.add_entity_config() gives the group of a YAML entity.
        group = hash(ID("group_mqtt"))
        sorting = [line for line in lines if "set_web_server_sorting" in line]
        self.assertEqual(len(sorting), 1)
        self.assertRegex(
            sorting[0],
            rf"^mqtt_subs->set_web_server_sorting\(web_server_base, {group}U?LL, 1\.0f\);$",
        )
        self.assertIn("USE_WEBSERVER_SORTING", {d.name for d in CORE.defines})


if __name__ == "__main__":
    unittest.main()
