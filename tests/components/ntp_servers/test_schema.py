"""The component's YAML schema: the SNTP clock it drives, and what it generates from it."""

from pathlib import Path
import unittest

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import loader
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_PLATFORM,
    PLATFORM_ESP32,
    PLATFORM_ESP8266,
    PLATFORM_HOST,
)
from esphome.core import CORE, ID
import esphome.final_validate as fv

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import ntp_servers  # noqa: E402

HEADER = (
    Path(__file__).resolve().parents[3] / "components/ntp_servers/ntp_servers.h"
).read_text()

SERVERS = ["0.pool.ntp.org", "1.pool.ntp.org", "2.pool.ntp.org"]
SNTP_TIME = {"platform": "sntp", "id": ID("sntp_time"), "servers": SERVERS}
HOST_TIME = {"platform": "host", "id": ID("sntp_time")}


def setUpModule():
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST


def validate(**options):
    return ntp_servers.CONFIG_SCHEMA({"sntp_id": "sntp_time", **options})


class Schema(unittest.TestCase):
    def test_the_clock_is_a_reference(self):
        config = validate()
        self.assertIsInstance(config["sntp_id"], ID)
        self.assertEqual(config["sntp_id"].id, "sntp_time")

    def test_the_clock_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            ntp_servers.CONFIG_SCHEMA({})

    def test_a_misspelled_option_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"):
            validate(server="ntp.local")

    def test_it_needs_a_time_component(self):
        self.assertIn("time", ntp_servers.DEPENDENCIES)


class Platforms(unittest.TestCase):
    def tearDown(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def test_esp32_is_accepted(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        validate()

    def test_another_platform_is_refused(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP8266
        with self.assertRaisesRegex(cv.Invalid, "only available on"):
            validate()


class FinalValidate(unittest.TestCase):
    def tearDown(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def check(self, full_config):
        token = fv.full_config.set(full_config)
        try:
            return ntp_servers.FINAL_VALIDATE_SCHEMA(validate())
        finally:
            fv.full_config.reset(token)

    def test_an_sntp_clock_passes(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        self.check({"time": [SNTP_TIME]})

    def test_another_clock_is_refused_on_the_device(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        with self.assertRaisesRegex(
            cv.Invalid,
            "sntp_id: sntp_time is a homeassistant time; the servers are the sntp platform's",
        ):
            self.check({"time": [{"platform": "homeassistant", "id": ID("sntp_time")}]})

    def test_the_host_suite_drives_a_host_clock(self):
        self.check({"time": [HOST_TIME]})


class Generated(unittest.TestCase):
    def tearDown(self):
        CORE.reset()
        setUpModule()

    def generate(self, clock):
        CORE.reset()
        setUpModule()
        config = validate(id="device_ntp")
        CORE.config = {"time": [clock]}

        async def declare():
            cg.new_Pvariable(
                ID("sntp_time", is_declaration=True, type=config["sntp_id"].type)
            )

        CORE.component_ids.add("device_ntp")
        CORE.add_job(ntp_servers.to_code, config)
        CORE.add_job(declare)
        CORE.flush_tasks()
        return [str(statement) for statement in CORE.main_statements]

    def test_the_clocks_servers_are_the_fallback_in_its_order(self):
        statements = self.generate(SNTP_TIME)
        self.assertIn("device_ntp->set_clock(sntp_time);", statements)
        fallback = [s for s in statements if "add_fallback_server" in s]
        self.assertEqual(
            fallback,
            [f'device_ntp->add_fallback_server("{server}");' for server in SERVERS],
        )
        self.assertEqual(len([s for s in statements if "set_preference_hash" in s]), 1)

    def test_a_clock_without_servers_gives_no_fallback(self):
        statements = self.generate(HOST_TIME)
        self.assertFalse([s for s in statements if "add_fallback_server" in s])

    def test_the_setters_it_calls_exist(self):
        # cg.MockObj answers to any attribute, so a renamed setter is otherwise a link error in
        # a full firmware build and nothing here.
        for setter in (
            "void set_clock(time::RealTimeClock *clock)",
            "void add_fallback_server(const char *server)",
            "void set_preference_hash(uint32_t hash)",
        ):
            with self.subTest(setter=setter):
                self.assertIn(setter, HEADER)


if __name__ == "__main__":
    unittest.main()
