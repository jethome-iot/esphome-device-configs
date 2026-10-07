"""The component's YAML schema: a GPIO output and its blink and pulse times, and the actions
that drive it."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import automation, loader
from esphome.config import path_context
from esphome.const import (
    KEY_CORE,
    KEY_TARGET_PLATFORM,
    KEY_VARIANT,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import CORE, Lambda

# Registering the pin schemas of the two platforms the tests validate for.
import esphome.components.esp32.gpio  # noqa: F401
import esphome.components.host.gpio  # noqa: F401
from esphome.components.esp32.const import KEY_BOARD, KEY_ESP32, VARIANT_ESP32

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import status_indicator  # noqa: E402

TIMES = (
    "slow_on_time",
    "slow_off_time",
    "fast_on_time",
    "fast_off_time",
    "pulse_duration",
)


def setUpModule():
    # A pin records the config path it was validated under.
    path_context.set(["status_indicator"])


def ms(period):
    return period.total_milliseconds


class Schema(unittest.TestCase):
    def setUp(self):
        CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_HOST

    def validate(self, **options):
        return status_indicator.CONFIG_SCHEMA({"pin": "GPIO2", **options})

    def test_a_pin_is_all_it_needs(self):
        config = self.validate()
        self.assertEqual(config["pin"]["number"], 2)
        self.assertTrue(config["pin"]["mode"]["output"])
        self.assertEqual(config["id"].type, status_indicator.StatusIndicator)
        self.assertEqual(
            {key: ms(config[key]) for key in TIMES},
            {
                "slow_on_time": 500,
                "slow_off_time": 500,
                "fast_on_time": 200,
                "fast_off_time": 200,
                "pulse_duration": 200,
            },
        )

    def test_it_takes_an_id(self):
        self.assertEqual(self.validate(id="red_led")["id"].id, "red_led")

    def test_the_pin_is_required(self):
        with self.assertRaisesRegex(
            cv.Invalid, r"required key not provided @ data\['pin'\]"
        ):
            status_indicator.CONFIG_SCHEMA({})

    def test_each_time_takes_any_unit(self):
        for key in TIMES:
            with self.subTest(key=key):
                self.assertEqual(ms(self.validate(**{key: "1.5s"})[key]), 1500)

    def test_a_zero_or_negative_time_is_refused(self):
        for key in TIMES:
            for value in ("0ms", "-100ms"):
                with (
                    self.subTest(key=key, value=value),
                    self.assertRaisesRegex(cv.Invalid, "value must be higher than 0s"),
                ):
                    self.validate(**{key: value})

    def test_a_time_without_a_unit_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, r"has no time \*unit\*"):
            self.validate(slow_on_time=500)

    def test_a_time_finer_than_a_millisecond_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Maximum precision is milliseconds"):
            self.validate(fast_on_time="1.5ms")

    def test_an_unknown_key_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"):
            self.validate(blink_time="1s")


class Esp32Pin(unittest.TestCase):
    def setUp(self):
        CORE.data.setdefault(KEY_CORE, {})[KEY_TARGET_PLATFORM] = PLATFORM_ESP32
        CORE.data[KEY_ESP32] = {KEY_VARIANT: VARIANT_ESP32, KEY_BOARD: "esp32dev"}

    def tearDown(self):
        CORE.data[KEY_CORE][KEY_TARGET_PLATFORM] = PLATFORM_HOST
        del CORE.data[KEY_ESP32]

    # The JXD CPU board's LED is on GPIO2, a strapping pin.
    def test_a_strapping_pin_warns_unless_told_not_to(self):
        with self.assertLogs(level="WARNING") as logs:
            status_indicator.CONFIG_SCHEMA({"pin": "GPIO2"})
        self.assertIn("GPIO2 is a strapping PIN", logs.output[0])
        with self.assertNoLogs(level="WARNING"):
            config = status_indicator.CONFIG_SCHEMA(
                {"pin": {"number": "GPIO2", "ignore_strapping_warning": True}}
            )
        self.assertTrue(config["pin"]["ignore_strapping_warning"])

    def test_an_input_only_pin_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "does not support output pin mode"):
            status_indicator.CONFIG_SCHEMA({"pin": "GPIO34"})


def action(name, value):
    return automation.ACTION_REGISTRY[f"status_indicator.{name}"].schema(value)


SIMPLE = ("turn_on", "turn_off", "blink_slow", "blink_fast", "blink_n", "pulse")


class Actions(unittest.TestCase):
    def test_they_are_registered(self):
        for name in (*SIMPLE, "set_state"):
            with self.subTest(name=name):
                self.assertIn(f"status_indicator.{name}", automation.ACTION_REGISTRY)

    def test_each_takes_the_id_on_its_own_or_as_a_key(self):
        for name in SIMPLE:
            for value in ("red_led", {"id": "red_led"}):
                with self.subTest(name=name, value=value):
                    config = action(name, value)
                    self.assertEqual(config["id"].id, "red_led")
                    self.assertFalse(config["id"].is_declaration)

    # The one instance, as upstream's actions find a component declared once.
    def test_each_works_without_an_id(self):
        for name in SIMPLE:
            for value in ({}, None):
                with self.subTest(name=name, value=value):
                    config = action(name, value)
                    self.assertIsNone(config["id"].id)
                    self.assertEqual(
                        config["id"].type, status_indicator.StatusIndicator
                    )

    def test_each_refuses_an_unknown_key(self):
        for name in (*SIMPLE, "set_state"):
            with (
                self.subTest(name=name),
                self.assertRaisesRegex(cv.Invalid, "extra keys not allowed"),
            ):
                action(name, {"id": "red_led", "state": "ON", "colour": "red"})

    def test_blink_n_defaults(self):
        config = action("blink_n", "red_led")
        self.assertEqual(config["count"], 3)
        self.assertEqual(ms(config["on_time"]), 200)
        self.assertEqual(ms(config["off_time"]), 200)
        self.assertEqual(ms(config["pause_time"]), 1500)

    def test_blink_n_takes_its_fields(self):
        config = action(
            "blink_n",
            {
                "id": "red_led",
                "count": 255,
                "on_time": "50ms",
                "off_time": "1s",
                "pause_time": "5s",
            },
        )
        self.assertEqual(config["count"], 255)
        self.assertEqual(ms(config["on_time"]), 50)
        self.assertEqual(ms(config["off_time"]), 1000)
        self.assertEqual(ms(config["pause_time"]), 5000)

    def test_blink_n_takes_a_lambda_for_each_field(self):
        for key in ("count", "on_time", "off_time", "pause_time"):
            with self.subTest(key=key):
                config = action("blink_n", {"id": "red_led", key: Lambda("return 2;")})
                self.assertIsInstance(config[key], Lambda)

    def test_blink_n_refuses_a_count_out_of_a_byte_or_zero(self):
        for count, message in (
            (0, "at least 1"),
            (256, "at most 255"),
            (-1, "at least 1"),
        ):
            with (
                self.subTest(count=count),
                self.assertRaisesRegex(cv.Invalid, message),
            ):
                action("blink_n", {"id": "red_led", "count": count})

    def test_blink_n_refuses_a_count_that_is_no_number(self):
        with self.assertRaisesRegex(cv.Invalid, "Expected integer"):
            action("blink_n", {"id": "red_led", "count": "many"})

    def test_blink_n_refuses_a_zero_time(self):
        for key in ("on_time", "off_time", "pause_time"):
            with (
                self.subTest(key=key),
                self.assertRaisesRegex(cv.Invalid, "value must be higher than 0s"),
            ):
                action("blink_n", {"id": "red_led", key: "0ms"})

    # Left out, the duration is the instance's pulse_duration, decided on the device.
    def test_pulse_has_no_default_duration(self):
        self.assertNotIn("duration", action("pulse", "red_led"))

    def test_pulse_takes_a_duration(self):
        config = action("pulse", {"id": "red_led", "duration": "2s"})
        self.assertEqual(ms(config["duration"]), 2000)
        config = action("pulse", {"id": "red_led", "duration": Lambda("return 10;")})
        self.assertIsInstance(config["duration"], Lambda)

    def test_pulse_refuses_a_zero_duration(self):
        with self.assertRaisesRegex(cv.Invalid, "value must be higher than 0s"):
            action("pulse", {"id": "red_led", "duration": "0s"})

    def test_set_state_takes_every_state_in_any_case(self):
        for state in status_indicator.INDICATOR_STATES:
            for spelling in (state, state.lower()):
                with self.subTest(state=spelling):
                    config = action("set_state", {"id": "red_led", "state": spelling})
                    self.assertEqual(config["state"], state)

    # YAML reads a bare ON or OFF as a boolean.
    def test_set_state_takes_on_and_off_as_booleans(self):
        for value, state in ((True, "ON"), (False, "OFF")):
            with self.subTest(value=value):
                config = action("set_state", {"id": "red_led", "state": value})
                self.assertEqual(config["state"], state)

    def test_set_state_takes_a_lambda(self):
        config = action(
            "set_state",
            {
                "id": "red_led",
                "state": Lambda("return status_indicator::IndicatorState::ON;"),
            },
        )
        self.assertIsInstance(config["state"], Lambda)

    def test_set_state_refuses_an_unknown_state(self):
        with self.assertRaisesRegex(cv.Invalid, "Unknown value 'BLINK'"):
            action("set_state", {"id": "red_led", "state": "blink"})

    def test_set_state_needs_a_state(self):
        with self.assertRaisesRegex(
            cv.Invalid, r"required key not provided @ data\['state'\]"
        ):
            action("set_state", {"id": "red_led"})
        with self.assertRaisesRegex(cv.Invalid, "expected a dictionary"):
            action("set_state", "red_led")


if __name__ == "__main__":
    unittest.main()
