"""The component's YAML schema, the mqtt: block it insists on, and codegen's entity walk."""

import unittest
from pathlib import Path

import yaml

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.const import (
    KEY_CORE,
    KEY_ESP32,
    KEY_TARGET_FRAMEWORK,
    KEY_TARGET_PLATFORM,
    KEY_VARIANT,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
)
from esphome.core import CORE, ID

ROOT = Path(__file__).resolve().parents[3]

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(ROOT / "components")

from esphome.components import mqtt  # noqa: E402  upstream's, which the devices build
from esphome.components import mqtt_config  # noqa: E402

NODE = "jxd-r6-e1eth-lcd"


class _AnyTag(yaml.SafeLoader):
    """Reads a package with ESPHome's tags (!extend, !include) as plain values."""


_AnyTag.add_multi_constructor("!", lambda loader_, suffix, node: None)


def shipped_block():
    """The mqtt: block devices/JXD/packages/features/mqtt.yaml ships."""
    package = ROOT / "devices" / "JXD" / "packages" / "features" / "mqtt.yaml"
    return yaml.load(package.read_text(), Loader=_AnyTag)["mqtt"]


def on_esp32():
    CORE.reset()
    CORE.name = NODE
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_ESP32
    CORE.data[KEY_CORE][KEY_TARGET_FRAMEWORK] = "esp-idf"
    # The idf_send_async default reads the chip.
    CORE.data[KEY_ESP32] = {KEY_VARIANT: "ESP32"}


class FinalValidate(unittest.TestCase):
    """Against upstream's real schema with its defaults, as the devices validate it."""

    def setUp(self):
        on_esp32()

    def mqtt_block(self, **changes):
        block = shipped_block()
        for key, value in changes.items():
            if value is ...:
                block.pop(key, None)
            else:
                block[key] = value
        return mqtt.CONFIG_SCHEMA(block)

    def final_validate(self, mqtt_conf):
        token = fv.full_config.set({"mqtt": mqtt_conf})
        try:
            return mqtt_config.FINAL_VALIDATE_SCHEMA({})
        finally:
            fv.full_config.reset(token)

    def assert_refused(self, message, **changes):
        block = self.mqtt_block(**changes)
        with self.assertRaises(cv.Invalid) as refused:
            self.final_validate(block)
        self.assertEqual(refused.exception.msg, message)
        self.assertEqual(refused.exception.path, ["mqtt"])

    def test_the_shipped_block_passes(self):
        self.final_validate(self.mqtt_block())

    def test_a_broker_in_yaml_is_refused(self):
        self.assert_refused(
            "mqtt_config sets the broker on the device: leave 'broker: \"\"' in the mqtt: block",
            broker="192.168.1.10",
        )

    def test_enable_on_boot_is_refused(self):
        self.assert_refused(
            "mqtt_config starts the client once it has a broker: set 'enable_on_boot: false'",
            enable_on_boot=True,
        )
        # Upstream's default is on.
        self.assert_refused(
            "mqtt_config starts the client once it has a broker: set 'enable_on_boot: false'",
            enable_on_boot=...,
        )

    def test_a_reboot_timeout_is_refused(self):
        message = (
            "mqtt_config needs 'reboot_timeout: 0s': a mistyped broker would reboot the "
            "device every 15 minutes"
        )
        self.assert_refused(message, reboot_timeout="1min")
        self.assert_refused(message, reboot_timeout=...)

    def test_a_kept_session_is_refused(self):
        message = "set 'clean_session: true': a kept session leaves removed topics subscribed on the broker"
        self.assert_refused(message, clean_session=False)
        # Upstream's default keeps it.
        self.assert_refused(message, clean_session=...)

    def test_discovery_must_be_compiled_on(self):
        message = "mqtt_config switches Home Assistant discovery on the device: set 'discovery: true'"
        self.assert_refused(message, discovery=False)
        self.assert_refused(message, discovery="CLEAN")

    def test_unretained_discovery_is_refused(self):
        self.assert_refused(
            "set 'discovery_retain: true': Home Assistant loses unretained entries when it restarts",
            discovery_retain=False,
        )

    def test_legacy_unique_ids_are_refused(self):
        message = "set 'discovery_unique_id_generator: mac': legacy ids collide between devices"
        self.assert_refused(message, discovery_unique_id_generator="legacy")
        self.assert_refused(message, discovery_unique_id_generator=...)

    def test_discover_ip_is_refused(self):
        message = (
            "set 'discover_ip: false': the native API already announces the device"
        )
        self.assert_refused(message, discover_ip=True)
        self.assert_refused(message, discover_ip=...)

    def test_a_log_topic_is_refused(self):
        message = "set 'log_topic: null': the logger runs at debug level and every line would go to the broker"
        self.assert_refused(
            message, log_topic=...
        )  # upstream's default is <prefix>/debug
        self.assert_refused(message, log_topic="some/debug")

    def test_waiting_for_a_connection_is_refused(self):
        self.assert_refused(
            "'wait_for_connection' would hold the boot until a broker nobody may have set answers",
            wait_for_connection=True,
        )

    def test_a_topic_prefix_in_yaml_is_refused(self):
        self.assert_refused(
            "the topic prefix is set on the device: leave 'topic_prefix' out",
            topic_prefix="home/jxd",
        )

    def test_a_client_id_in_yaml_is_refused(self):
        self.assert_refused(
            "the client ID is set on the device: leave 'client_id' out",
            client_id="jxd",
        )

    def test_a_status_topic_of_its_own_is_refused(self):
        for key in ("birth_message", "will_message", "shutdown_message"):
            with self.subTest(key=key):
                self.assert_refused(
                    f"mqtt_config sends '{key}' to each device's '<topic prefix>/status': "
                    f"set its 'topic' to '{NODE}/status' or leave the block out",
                    **{key: {"topic": "somewhere/else", "payload": "x"}},
                )

    def test_status_messages_with_their_own_payload_or_none_at_all_pass(self):
        self.final_validate(
            self.mqtt_block(
                birth_message={"topic": f"{NODE}/status", "payload": "up"},
                will_message=None,
                shutdown_message=None,
            )
        )

    def test_tls_is_refused(self):
        self.assert_refused(
            "mqtt_config has no TLS yet: remove 'certificate_authority'",
            certificate_authority="-----BEGIN CERTIFICATE-----",
        )
        self.assert_refused(
            "mqtt_config has no TLS yet: remove 'client_certificate'",
            client_certificate="cert",
            client_certificate_key="key",
        )

    def test_the_first_fault_is_the_answer(self):
        self.assert_refused(
            "mqtt_config sets the broker on the device: leave 'broker: \"\"' in the mqtt: block",
            broker="b",
            enable_on_boot=True,
            client_id="x",
        )


class Schema(unittest.TestCase):
    def setUp(self):
        CORE.reset()
        CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def test_it_takes_an_id_and_finds_the_client(self):
        config = mqtt_config.CONFIG_SCHEMA({"id": "mqtt_settings"})
        self.assertEqual(config["id"].id, "mqtt_settings")
        self.assertIn(mqtt_config.CONF_MQTT_PARENT_ID, config)

    def test_an_unknown_option_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "broker"):
            mqtt_config.CONFIG_SCHEMA({"broker": "b"})

    def test_esp32_is_accepted(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        mqtt_config.CONFIG_SCHEMA({})

    # ESP32 for the devices and host for the suite; NVS and RTC memory are ESP32's.
    def test_another_platform_is_refused(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP8266
        with self.assertRaisesRegex(cv.Invalid, "only available on"):
            mqtt_config.CONFIG_SCHEMA({})


def _id(name):
    return ID(name, is_declaration=True)


class EntityWalk(unittest.TestCase):
    def test_every_entity_with_an_mqtt_component_in_config_order(self):
        config = {
            "esphome": {"name": NODE},
            "switch": [
                {
                    "id": _id("relay_1"),
                    "mqtt_id": _id("relay_1_mqtt"),
                    "name": "Relay 1",
                },
                {"id": _id("hidden"), "mqtt_id": _id("hidden_mqtt"), "state_topic": ""},
            ],
            "sensor": [
                {
                    "id": _id("vin"),
                    "mqtt_id": _id("vin_mqtt"),
                    # A nested entity, as a platform with sub-sensors has.
                    "voltage": {"id": _id("volts"), "mqtt_id": _id("volts_mqtt")},
                }
            ],
            "select": {"id": _id("mode"), "mqtt_id": _id("mode_mqtt")},
            # Not entities: an id without an MQTT component, or one that is not an ID.
            "dallas_scan": {"id": _id("temps")},
            "text_sensor": [{"id": _id("ip"), "mqtt_id": "not_an_id"}],
        }
        walked = [(m.id, e.id) for m, e in mqtt_config.mqtt_entities(config)]
        self.assertEqual(
            walked,
            [
                ("relay_1_mqtt", "relay_1"),
                ("hidden_mqtt", "hidden"),
                ("vin_mqtt", "vin"),
                ("volts_mqtt", "volts"),
                ("mode_mqtt", "mode"),
            ],
        )

    def test_nothing_to_walk(self):
        self.assertEqual(list(mqtt_config.mqtt_entities({"esphome": {}})), [])


if __name__ == "__main__":
    unittest.main()
