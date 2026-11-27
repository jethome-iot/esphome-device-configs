"""The component's YAML schema: what it refuses, and with which message."""

import re
import unittest
from pathlib import Path

import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import KEY_CORE, KEY_TARGET_PLATFORM, PLATFORM_ESP32
from esphome.core import CORE
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import automations  # noqa: E402


class FolderPath(unittest.TestCase):
    def test_a_folder_name_passes(self):
        self.assertEqual(automations.folder_name("rules"), "rules")

    def test_anything_but_a_folder_name_is_refused(self):
        for value in ("", "a/b", "/abs", "..", ".", "a\\b"):
            with (
                self.subTest(value=value),
                self.assertRaisesRegex(
                    cv.Invalid, "folder_path must be a single folder name"
                ),
            ):
                automations.folder_name(value)

    def test_the_default_is_a_folder_name(self):
        config = automations.CONFIG_SCHEMA({"storage": "user_storage"})
        self.assertEqual(config[automations.CONF_FOLDER_PATH], "automations")


class LoopTaskStack(unittest.TestCase):
    """A config imported before #345 keeps the 8192 B stack: the build warns and goes on."""

    def setUp(self):
        self.saved = CORE.data.get(KEY_CORE)
        CORE.data[KEY_CORE] = {KEY_TARGET_PLATFORM: PLATFORM_ESP32}

    def tearDown(self):
        if self.saved is None:
            CORE.data.pop(KEY_CORE, None)
        else:
            CORE.data[KEY_CORE] = self.saved

    def validate(self, full):
        token = fv.full_config.set(full)
        try:
            return automations._validate_loop_task_stack({"kept": True})
        finally:
            fv.full_config.reset(token)

    @staticmethod
    def esp32(size):
        return {"framework": {"advanced": {"loop_task_stack_size": size}}}

    def test_the_default_stack_with_web_server_warns_and_passes(self):
        with self.assertLogs(automations.__name__, level="WARNING") as logs:
            config = self.validate({"web_server": {}, "esp32": self.esp32(8192)})
        self.assertEqual(config, {"kept": True})
        self.assertEqual(len(logs.output), 1)
        self.assertIn(
            "need 'esp32: framework: advanced: loop_task_stack_size: 12288' or more, "
            "not 8192: rules nested 8 deep can overflow a smaller loop task stack",
            logs.output[0],
        )

    def test_a_config_with_no_advanced_block_warns(self):
        with self.assertLogs(automations.__name__, level="WARNING") as logs:
            self.validate({"web_server": {}, "esp32": {"framework": {}}})
        self.assertIn("loop_task_stack_size: 12288", logs.output[0])

    def test_12288_is_quiet(self):
        with self.assertNoLogs(automations.__name__, level="WARNING"):
            config = self.validate({"web_server": {}, "esp32": self.esp32(12288)})
        self.assertEqual(config, {"kept": True})

    def test_without_web_server_the_default_is_quiet(self):
        with self.assertNoLogs(automations.__name__, level="WARNING"):
            self.validate({"esp32": self.esp32(8192)})

    def test_another_platform_is_not_checked(self):
        CORE.data[KEY_CORE] = {KEY_TARGET_PLATFORM: "host"}
        with self.assertNoLogs(automations.__name__, level="WARNING"):
            self.validate({"web_server": {}})


class OurBoardMeetsTheFloor(unittest.TestCase):
    """The warning does not fail CI: our own firmwares are held to the floor here."""

    def test_the_cpu_board_package_gives_the_loop_task_the_floor(self):
        board = (
            Path(__file__).resolve().parents[3]
            / "devices/JXD/packages/boards/jxd-cpu-e1eth.yaml"
        ).read_text()
        found = re.findall(
            r"^\s+loop_task_stack_size:\s*(\d+)\s*$", board, re.MULTILINE
        )
        self.assertEqual(
            len(found), 1, "one loop_task_stack_size in the CPU board package"
        )
        self.assertGreaterEqual(int(found[0]), automations.MIN_LOOP_TASK_STACK_SIZE)


if __name__ == "__main__":
    unittest.main()
