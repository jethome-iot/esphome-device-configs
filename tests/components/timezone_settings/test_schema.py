"""The component's YAML schema: the compiled zone, the global it keeps Home Assistant's zone in,
the clocks, and what it generates from them."""

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

from esphome.components import timezone_settings  # noqa: E402

HEADER = (
    Path(__file__).resolve().parents[3]
    / "components/timezone_settings/timezone_settings.h"
).read_text()

GLOBAL = {
    "id": ID("stored_timezone", is_declaration=True),
    "type": "esphome::time::ParsedTimezone",
    "restore_value": True,
}
HA_TIME = {"platform": "homeassistant", "id": ID("homeassistant_time")}
SNTP_TIME = {"platform": "sntp", "id": ID("sntp_time"), "timezone": "UTC0"}


def setUpModule():
    CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST


def validate(**options):
    return timezone_settings.CONFIG_SCHEMA({"timezone": "UTC", **options})


class CompiledZone(unittest.TestCase):
    def test_a_name_is_kept_as_written(self):
        # As written: /api/device/time reports it, and the POSIX string is derived again.
        for zone in (
            "UTC",
            "Europe/Berlin",
            "America/Argentina/Buenos_Aires",
            "Etc/GMT+3",
        ):
            with self.subTest(zone=zone):
                self.assertEqual(validate(timezone=zone)["timezone"], zone)

    def test_a_posix_string_is_taken_too(self):
        zone = "CET-1CEST,M3.5.0,M10.5.0/3"
        self.assertEqual(validate(timezone=zone)["timezone"], zone)

    def test_it_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            timezone_settings.CONFIG_SCHEMA({})

    def test_an_empty_zone_is_refused(self):
        # time: reads an empty one as no timezone at all; a device needs one.
        with self.assertRaisesRegex(cv.Invalid, "timezone must name a zone"):
            validate(timezone="")

    def test_a_zone_tzdata_does_not_know_is_refused_as_time_refuses_it(self):
        for zone in ("Europe/Berln", "Mars/Olympus_Mons", "XYZ", "+3"):
            with (
                self.subTest(zone=zone),
                self.assertRaisesRegex(cv.Invalid, "Invalid POSIX timezone string"),
            ):
                validate(timezone=zone)

    def test_a_zone_the_device_could_not_parse_is_refused(self):
        # Python's parser takes other scripts' letters and digits; the device's does not.
        for zone in ("EST٣", "ESTé5"):
            with (
                self.subTest(zone=zone),
                self.assertRaisesRegex(cv.Invalid, "must be printable ASCII"),
            ):
                validate(timezone=zone)

    def test_an_offset_past_32_bits_is_refused(self):
        for zone in (
            "EST596524",
            "EST5EDT,M3.2.0/596524,M11.1.0",
            "EST-596523:14:08EDT",
        ):
            with (
                self.subTest(zone=zone),
                self.assertRaisesRegex(cv.Invalid, "has an offset past 596523 hours"),
            ):
                validate(timezone=zone)

    def test_not_a_string_is_refused(self):
        with self.assertRaises(cv.Invalid):
            validate(timezone=3)


class Wiring(unittest.TestCase):
    def test_the_global_and_the_clocks_are_references(self):
        config = validate(
            homeassistant_zone="stored_timezone",
            clocks={
                "homeassistant": "homeassistant_time",
                "sntp": "sntp_time",
                "rtc": "pcf8563_time",
            },
        )
        zone = config["homeassistant_zone"]
        self.assertEqual(zone.type, "globals::GlobalsComponent")
        self.assertFalse(zone.is_declaration)
        for key in ("homeassistant", "sntp", "rtc"):
            self.assertEqual(config["clocks"][key].type, "time::RealTimeClock")

    def test_everything_but_the_zone_is_optional(self):
        config = validate()
        self.assertNotIn("homeassistant_zone", config)
        self.assertNotIn("clocks", config)
        self.assertEqual(config["id"].type, "timezone_settings::TimezoneSettings")

    def test_home_assistants_zone_needs_home_assistants_clock(self):
        with self.assertRaisesRegex(
            cv.Invalid, "homeassistant_zone needs clocks: homeassistant"
        ):
            validate(homeassistant_zone="stored_timezone")
        with self.assertRaisesRegex(
            cv.Invalid, "homeassistant_zone needs clocks: homeassistant"
        ):
            validate(homeassistant_zone="stored_timezone", clocks={"sntp": "sntp_time"})

    def test_home_assistants_clock_alone_is_fine(self):
        # Its zone is then kept until the next reboot only.
        validate(clocks={"homeassistant": "homeassistant_time"})

    def test_an_unknown_clock_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"):
            validate(clocks={"gps": "gps_time"})

    def test_a_misspelled_option_is_refused(self):
        for key in ("time_zone", "homeassistant", "zone", "clock"):
            with (
                self.subTest(key=key),
                self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"),
            ):
                validate(**{key: "x"})

    def test_it_needs_a_time_component(self):
        self.assertEqual(timezone_settings.DEPENDENCIES, ["time"])


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
    def check(self, config, full_config):
        token = fv.full_config.set(full_config)
        try:
            return timezone_settings.FINAL_VALIDATE_SCHEMA(config)
        finally:
            fv.full_config.reset(token)

    def config(self, **options):
        return validate(
            homeassistant_zone="stored_timezone",
            clocks={"homeassistant": "homeassistant_time", "sntp": "sntp_time"},
            **options,
        )

    def test_the_device_wiring_passes(self):
        self.check(self.config(), {"globals": [GLOBAL], "time": [HA_TIME, SNTP_TIME]})

    def test_the_global_type_may_be_spelled_without_the_namespace(self):
        for spelling in ("time::ParsedTimezone", "::esphome::time::ParsedTimezone"):
            with self.subTest(spelling=spelling):
                self.check(
                    self.config(),
                    {"globals": [{**GLOBAL, "type": spelling}], "time": [HA_TIME]},
                )

    def test_a_global_of_another_type_is_refused(self):
        with self.assertRaisesRegex(
            cv.Invalid,
            "stored_timezone must be a global of type esphome::time::ParsedTimezone "
            "with restore_value: true",
        ):
            self.check(
                self.config(),
                {"globals": [{**GLOBAL, "type": "int"}], "time": [HA_TIME]},
            )

    def test_a_global_that_does_not_restore_is_refused(self):
        # Home Assistant's zone would be gone at every reboot until it answers again.
        with self.assertRaisesRegex(cv.Invalid, "with restore_value: true"):
            self.check(
                self.config(),
                {"globals": [{**GLOBAL, "restore_value": False}], "time": [HA_TIME]},
            )

    def test_a_missing_global_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "must be a global of type"):
            self.check(self.config(), {"time": [HA_TIME]})

    def test_home_assistants_clock_with_a_zone_of_its_own_is_refused(self):
        # USE_HOMEASSISTANT_TIMEZONE is then off, and Home Assistant's zone never arrives.
        with self.assertRaisesRegex(
            cv.Invalid,
            "homeassistant_time has a timezone of its own; Home Assistant sends its zone "
            "only to a homeassistant time without one",
        ):
            self.check(
                self.config(),
                {"globals": [GLOBAL], "time": [{**HA_TIME, "timezone": "UTC0"}]},
            )

    def test_the_other_clocks_may_have_one(self):
        self.check(self.config(), {"globals": [GLOBAL], "time": [HA_TIME, SNTP_TIME]})

    def test_nothing_to_check_without_them(self):
        self.check(validate(), {})


class Generated(unittest.TestCase):
    def tearDown(self):
        CORE.reset()
        setUpModule()

    def generate(self, config):
        CORE.reset()
        setUpModule()
        declared = [config["id"]]
        if "homeassistant_zone" in config:
            declared.append(
                ID(
                    "stored_timezone",
                    is_declaration=True,
                    type=config["homeassistant_zone"].type,
                )
            )
        for clock in config.get("clocks", {}).values():
            declared.append(ID(clock.id, is_declaration=True, type=clock.type))

        async def declare():
            for variable in declared[1:]:
                cg.new_Pvariable(variable)

        CORE.component_ids.add(config["id"].id)
        CORE.add_job(timezone_settings.to_code, config)
        CORE.add_job(declare)
        CORE.flush_tasks()
        return {define.name for define in CORE.defines}, [
            str(statement) for statement in CORE.main_statements
        ]

    def test_the_device_wiring(self):
        config = validate(
            id="device_timezone",
            timezone="Europe/Berlin",
            homeassistant_zone="stored_timezone",
            clocks={"homeassistant": "homeassistant_time", "rtc": "pcf8563_time"},
        )
        defines, statements = self.generate(config)
        self.assertIn("USE_TIME_TIMEZONE", defines)
        self.assertIn(
            'device_timezone->set_default_zone("Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3");',
            statements,
        )
        self.assertIn(
            "device_timezone->set_homeassistant_zone(&stored_timezone->value());",
            statements,
        )
        self.assertIn(
            "device_timezone->add_clock(homeassistant_time, "
            "timezone_settings::ClockSource::HOMEASSISTANT);",
            statements,
        )
        self.assertIn(
            "device_timezone->add_clock(pcf8563_time, timezone_settings::ClockSource::RTC);",
            statements,
        )
        hashes = [s for s in statements if "set_preference_hash" in s]
        self.assertEqual(len(hashes), 1)

    def test_a_posix_zone_is_its_own_posix_string(self):
        config = validate(id="tz", timezone="<+07>-7")
        _, statements = self.generate(config)
        self.assertIn('tz->set_default_zone("<+07>-7", "<+07>-7");', statements)
        self.assertFalse([s for s in statements if "set_homeassistant_zone" in s])
        self.assertFalse([s for s in statements if "add_clock" in s])

    def test_the_setters_it_calls_exist(self):
        # cg.MockObj answers to any attribute, so a renamed setter is otherwise a link error in
        # a full firmware build and nothing here.
        for setter in (
            "void set_default_zone(const char *zone, const char *posix)",
            "void set_homeassistant_zone(time::ParsedTimezone *zone)",
            "void set_preference_hash(uint32_t hash)",
            "void add_clock(time::RealTimeClock *clock, ClockSource source)",
            "enum class ClockSource : uint8_t { NONE, HOMEASSISTANT, SNTP, RTC }",
        ):
            with self.subTest(setter=setter):
                self.assertIn(setter, HEADER)


if __name__ == "__main__":
    unittest.main()
