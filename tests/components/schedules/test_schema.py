"""The component's YAML schema: what it accepts, what it refuses, and with which message."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import loader
from esphome.core import ID

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import schedules  # noqa: E402

BASE = {"storage": "user_storage", "time_id": "pcf8563_time"}


class Defaults(unittest.TestCase):
    def test_a_storage_and_a_clock_are_enough(self):
        config = schedules.CONFIG_SCHEMA(dict(BASE))
        self.assertEqual(config[schedules.CONF_FOLDER_PATH], "schedules")
        self.assertEqual(config[schedules.CONF_MAX_SCHEDULES], 16)

    def test_the_storage_and_the_clock_are_required(self):
        for missing in ("storage", "time_id"):
            config = {k: v for k, v in BASE.items() if k != missing}
            with (
                self.subTest(missing=missing),
                self.assertRaisesRegex(
                    cv.Invalid, f"required key not provided @ data\\['{missing}'\\]"
                ),
            ):
                schedules.CONFIG_SCHEMA(config)

    def test_climate_hub_is_not_pulled_in(self):
        # Without it an action on a thermostat is skipped, which the engine reports.
        self.assertEqual(schedules.AUTO_LOAD, ["json", "loop_job", "switch_hold"])
        self.assertEqual(
            schedules.DEPENDENCIES, ["filesystem_storage_abstract", "time"]
        )


class Id(unittest.TestCase):
    def test_the_namespace_is_no_id(self):
        with self.assertRaisesRegex(
            cv.Invalid,
            "id 'schedules' is the component's C\\+\\+ namespace; name it schedules_engine",
        ):
            schedules.CONFIG_SCHEMA({**BASE, "id": "schedules"})

    def test_any_other_id_passes(self):
        config = schedules.CONFIG_SCHEMA({**BASE, "id": "schedules_engine"})
        self.assertEqual(config["id"].id, "schedules_engine")


class FolderPath(unittest.TestCase):
    def test_a_folder_name_passes(self):
        self.assertEqual(schedules.folder_name("timetables"), "timetables")

    def test_anything_but_a_folder_name_is_refused(self):
        for value in ("", "a/b", "/abs", "..", ".", "a\\b"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "folder_path must be a single folder name"
                ),
            ):
                schedules.folder_name(value)


class MaxSchedules(unittest.TestCase):
    def test_the_range_is_one_to_thirty_two(self):
        for value in (1, 32):
            config = schedules.CONFIG_SCHEMA({**BASE, "max_schedules": value})
            self.assertEqual(config[schedules.CONF_MAX_SCHEDULES], value)

    def test_outside_the_range_is_refused(self):
        for value, message in ((0, "at least 1"), (33, "at most 32")):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(cv.Invalid, message),
            ):
                schedules.CONFIG_SCHEMA({**BASE, "max_schedules": value})


class FinalValidate(unittest.TestCase):
    def validate(self, full_config, folder="schedules", storage="user_storage"):
        config = {"storage": ID(storage), "folder_path": folder}
        token = fv.full_config.set(full_config)
        try:
            return schedules.FINAL_VALIDATE_SCHEMA(config)
        finally:
            fv.full_config.reset(token)

    def test_a_folder_of_its_own_passes(self):
        self.validate({})
        self.validate(
            {
                "climate_hub": {
                    "storage": ID("user_storage"),
                    "folder_path": "climates",
                },
                "automations": {
                    "storage": ID("user_storage"),
                    "folder_path": "automations",
                },
            }
        )

    def test_another_components_folder_is_refused(self):
        for owner, folder in (("climate_hub", "climates"), ("automations", "rules")):
            with (
                self.subTest(owner=owner),
                self.assertRaisesRegex(
                    cv.Invalid, f"folder_path '{folder}' is {owner}'s folder"
                ),
            ):
                self.validate(
                    {owner: {"storage": ID("user_storage"), "folder_path": folder}},
                    folder=folder,
                )

    def test_a_default_folder_counts_too(self):
        with self.assertRaisesRegex(
            cv.Invalid, "folder_path 'climates' is climate_hub's folder"
        ):
            self.validate(
                {"climate_hub": {"storage": ID("user_storage")}}, folder="climates"
            )

    def test_the_same_name_on_another_storage_passes(self):
        self.validate(
            {"climate_hub": {"storage": ID("sd_card"), "folder_path": "climates"}},
            folder="climates",
        )


if __name__ == "__main__":
    unittest.main()
