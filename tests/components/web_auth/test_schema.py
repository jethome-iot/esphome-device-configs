"""The component's YAML schema: the web_server block it cannot work without."""

import re
import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_PLATFORM,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
)
from esphome.core import CORE

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import web_auth  # noqa: E402

DIGEST = {
    "web_server": {"auth": {"username": "admin", "password": "admin", "type": "digest"}}
}
BASIC = {
    "web_server": {"auth": {"username": "admin", "password": "admin", "type": "basic"}}
}


class FinalValidate(unittest.TestCase):
    def setUp(self):
        CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def validate(self, full_config):
        token = fv.full_config.set(full_config)
        try:
            return web_auth.FINAL_VALIDATE_SCHEMA({})
        finally:
            fv.full_config.reset(token)

    def test_a_digest_block_is_what_it_wants(self):
        self.validate(DIGEST)

    def test_no_web_server_is_refused(self):
        with self.assertRaisesRegex(
            cv.Invalid, "needs a web_server with an 'auth:' block"
        ):
            self.validate({})

    # Without auth: the middleware is preprocessed out, so there is nothing to replace and
    # the component would silently do nothing.
    def test_a_web_server_without_auth_is_refused(self):
        with self.assertRaisesRegex(
            cv.Invalid, "needs a web_server with an 'auth:' block"
        ):
            self.validate({"web_server": {"port": 80}})

    # Off ESP32 upstream takes the basic-auth path, which compares a hash fixed at build time.
    def test_basic_auth_is_refused_off_esp32(self):
        with self.assertRaisesRegex(
            cv.Invalid, "needs 'auth:' with 'type: digest' off ESP32"
        ):
            self.validate(BASIC)

    def test_basic_auth_is_accepted_on_esp32(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        self.validate(BASIC)

    # The compiled pair never goes through WebAuth::validate: it is published at boot and it
    # is what a factory reset restores, so a pair the device would refuse locks it for good.
    def test_a_factory_pair_the_component_would_refuse_is_caught_here(self):
        for field, value, fault in (
            ("username", "u" * 33, "is over 32 characters"),
            ("password", "p" * 65, "is over 64 characters"),
            ("username", "админ", "must be printable ASCII"),
            ("password", "pass\tword", "must be printable ASCII"),
            ("username", 'adm"in', "cannot contain ':', '\"' or '\\'"),
            ("username", "adm:in", "cannot contain ':', '\"' or '\\'"),
            ("username", "adm\\in", "cannot contain ':', '\"' or '\\'"),
        ):
            with self.subTest(field=field, value=value):
                config = {"web_server": {"auth": dict(DIGEST["web_server"]["auth"])}}
                config["web_server"]["auth"][field] = value
                with self.assertRaisesRegex(cv.Invalid, re.escape(f"{field} {fault}")):
                    self.validate(config)

    # A password carries none of the reserved characters into a header, so it may hold them.
    def test_a_factory_password_may_carry_the_reserved_characters(self):
        config = {"web_server": {"auth": dict(DIGEST["web_server"]["auth"])}}
        config["web_server"]["auth"]["password"] = 'pa:ss"w\\ord'
        self.validate(config)


class Schema(unittest.TestCase):
    def setUp(self):
        CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def test_it_takes_no_options(self):
        config = web_auth.CONFIG_SCHEMA({})
        self.assertIn("web_server_base_id", config)

    def test_an_unknown_option_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "username"):
            web_auth.CONFIG_SCHEMA({"username": "admin"})

    # ESP32 for the devices and host for this suite; the pointers the component replaces are
    # web_server_base's ESP-IDF ones, and nowhere else keeps the pair that way.
    def test_another_platform_is_refused(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP8266
        with self.assertRaisesRegex(cv.Invalid, "only available on"):
            web_auth.CONFIG_SCHEMA({})


if __name__ == "__main__":
    unittest.main()
