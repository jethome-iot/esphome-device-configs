"""The update platform's schema: the source it needs and the channels it refuses."""

import itertools
import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components.jethome_update import update as jethome_update  # noqa: E402

SOURCE = "https://fw.jethome.com/api/devices/jxd-r6-e1eth-lcd/info"
_NAMES = itertools.count()


def validate(**overrides):
    """One validated config. Names are unique: upstream refuses a name it has already seen."""
    config = {"name": f"Firmware update {next(_NAMES)}", "source": SOURCE}
    config.update(overrides)
    return jethome_update.CONFIG_SCHEMA(config)


class Schema(unittest.TestCase):
    def test_the_channel_defaults_to_release(self):
        self.assertEqual(validate()["channel"], "release")

    def test_the_poll_interval_defaults_to_six_hours(self):
        self.assertEqual(validate()["update_interval"], cv.TimePeriod(hours=6))

    def test_a_channel_is_kept_as_written(self):
        self.assertEqual(validate(channel="nightly")["channel"], "nightly")

    def test_a_channel_that_is_not_a_bare_word_is_refused(self):
        for value in ("", " ", "release channel", " release"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, "without spaces"),
            ):
                validate(channel=value)

    def test_the_source_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            jethome_update.CONFIG_SCHEMA({"name": f"Firmware update {next(_NAMES)}"})

    def test_a_source_that_is_not_a_url_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "URL scheme"):
            validate(source="fw.jethome.com")


if __name__ == "__main__":
    unittest.main()
