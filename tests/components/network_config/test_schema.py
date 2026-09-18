"""The component's YAML schema: what it fills in, what it refuses, and what it forces on upstream."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.const import KEY_CORE, KEY_TARGET_PLATFORM, PLATFORM_ESP8266, PLATFORM_HOST
from esphome.core import CORE

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
COMPONENTS = Path(__file__).resolve().parents[3] / "components"
loader.install_meta_finder(COMPONENTS)

from esphome.components import network_config  # noqa: E402


class Settings(unittest.TestCase):
    def setUp(self):
        # only_on() reads the target platform; the suite builds for host.
        CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def test_the_ids_are_generated(self):
        config = network_config.CONFIG_SCHEMA({})
        self.assertIn(network_config.CONF_CONFIG_JSON_ID, config)
        self.assertIn(network_config.CONF_SETTINGS_ID, config)
        self.assertIn("id", config)

    def test_the_interfaces_are_optional(self):
        config = network_config.CONFIG_SCHEMA({})
        self.assertNotIn(network_config.CONF_WIFI_ID, config)
        self.assertNotIn(network_config.CONF_ETHERNET_ID, config)

    def test_a_misspelled_option_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"):
            network_config.CONFIG_SCHEMA({"wifi": "wifi_id"})

    def test_the_platform_gate(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP8266
        with self.assertRaisesRegex(cv.Invalid, "only available on"):
            network_config.CONFIG_SCHEMA({})


class FinalValidate(unittest.TestCase):
    def validate(self, config, full_config):
        token = fv.full_config.set(full_config)
        try:
            return network_config.FINAL_VALIDATE_SCHEMA(config)
        finally:
            fv.full_config.reset(token)

    def test_a_config_with_no_interface_is_fine(self):
        self.validate({}, {})

    def test_both_interfaces_have_to_be_wired_in(self):
        with self.assertRaisesRegex(cv.Invalid, "needs 'wifi_id:'"):
            self.validate({}, {"wifi": {"enable_on_boot": False}})
        with self.assertRaisesRegex(cv.Invalid, "needs 'ethernet_id:'"):
            self.validate({}, {"ethernet": {}})
        self.validate(
            {"wifi_id": "wifi_id", "ethernet_id": "ethernet_id"},
            {"wifi": {"enable_on_boot": False}, "ethernet": {}},
        )

    def test_wifi_has_to_start_disabled(self):
        # start() replaces the whole station list, so it must not run before the boot script
        # can reinstall it.
        with self.assertRaisesRegex(cv.Invalid, "'enable_on_boot: false'"):
            self.validate({"wifi_id": "wifi_id"}, {"wifi": {"enable_on_boot": True}})
        with self.assertRaisesRegex(cv.Invalid, "'enable_on_boot: false'"):
            self.validate({"wifi_id": "wifi_id"}, {"wifi": {}})

    def test_the_yaml_must_not_list_networks(self):
        with self.assertRaisesRegex(cv.Invalid, "remove 'networks:'"):
            self.validate(
                {"wifi_id": "wifi_id"},
                {"wifi": {"enable_on_boot": False, "networks": [{"ssid": "home"}]}},
            )
        # An empty list is what upstream leaves behind for an ap-only config.
        self.validate(
            {"wifi_id": "wifi_id"},
            {"wifi": {"enable_on_boot": False, "networks": []}},
        )

    def test_neither_interface_may_carry_a_manual_ip(self):
        with self.assertRaisesRegex(cv.Invalid, "remove 'manual_ip:' from 'wifi:'"):
            self.validate(
                {"wifi_id": "wifi_id"},
                {"wifi": {"enable_on_boot": False, "manual_ip": {}}},
            )
        with self.assertRaisesRegex(cv.Invalid, "remove 'manual_ip:' from 'ethernet:'"):
            self.validate(
                {"ethernet_id": "ethernet_id"}, {"ethernet": {"manual_ip": {}}}
            )


class ForcedDefines(unittest.TestCase):
    """The manual-IP setters exist only behind a define upstream emits for its own YAML."""

    def test_both_defines_are_forced(self):
        source = (COMPONENTS / "network_config" / "__init__.py").read_text()
        self.assertIn('cg.add_define("USE_WIFI_MANUAL_IP")', source)
        self.assertIn('cg.add_define("USE_ETHERNET_MANUAL_IP")', source)


if __name__ == "__main__":
    unittest.main()
