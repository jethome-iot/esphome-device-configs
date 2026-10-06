"""The component's YAML schema: what it needs and what it refuses."""

import unittest
from pathlib import Path

import esphome.config_validation as cv
from esphome import loader

# External components import each other as esphome.components.<name>: give them the finder
# that external_components: installs when a config names the directory.
loader.install_meta_finder(Path(__file__).resolve().parents[3] / "components")

from esphome.components import meter_calibration  # noqa: E402

PM380 = {
    "board_info_id": "meter_board_info",
    "chip": "atm90e32",
    "board_model": "JXD-D3-PM3-3",
}
PM220 = {
    "board_info_id": "meter_board_info",
    "chip": "bl0906",
    "board_model": "JXD-D3-PM1-6",
}


def validate(**overrides):
    config = dict(PM380)
    for key, value in overrides.items():
        if value is None:
            config.pop(key)
        else:
            config[key] = value
    return meter_calibration.CONFIG_SCHEMA(config)


class Schema(unittest.TestCase):
    def test_a_complete_config_passes(self):
        config = validate()
        self.assertEqual(
            str(config[meter_calibration.CONF_BOARD_INFO_ID].id), "meter_board_info"
        )
        self.assertEqual(config[meter_calibration.CONF_CHIP], "atm90e32")
        self.assertEqual(config[meter_calibration.CONF_BOARD_MODEL], "JXD-D3-PM3-3")

    def test_the_board_info_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            validate(board_info_id=None)

    def test_the_board_info_is_a_reference(self):
        config = validate()
        board = config[meter_calibration.CONF_BOARD_INFO_ID]
        self.assertEqual(board.type, "jethome_board_info::JetHomeBoardInfo")
        self.assertFalse(board.is_declaration)

    def test_the_chip_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            validate(chip=None)

    def test_an_unknown_chip_is_refused(self):
        with self.assertRaisesRegex(
            cv.Invalid,
            "Unknown value 'ade7953', valid options are 'atm90e32', 'bl0906'",
        ):
            validate(chip="ade7953")

    def test_the_chip_is_not_case_sensitive(self):
        self.assertEqual(
            validate(chip="ATM90E32")[meter_calibration.CONF_CHIP], "atm90e32"
        )

    def test_the_board_model_is_required(self):
        with self.assertRaisesRegex(cv.Invalid, "required key not provided"):
            validate(board_model=None)

    def test_an_empty_board_model_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "board_model must not be empty"):
            validate(board_model="")

    def test_a_board_model_fills_at_most_the_records_field(self):
        self.assertEqual(
            validate(board_model="M" * 32)[meter_calibration.CONF_BOARD_MODEL], "M" * 32
        )
        with self.assertRaisesRegex(
            cv.Invalid, "board_model is at most 32 characters, the record's field"
        ):
            validate(board_model="M" * 33)

    def test_a_board_model_is_printable_ascii(self):
        for model in ("JXD-D3-PM3-3\n", "ЖXD-D3", "JXD\x00D3"):
            with (
                self.subTest(model=model),
                self.assertRaisesRegex(
                    cv.Invalid, "board_model must be printable ASCII"
                ),
            ):
                validate(board_model=model)

    def test_a_board_model_that_is_not_a_string_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "Must be string"):
            validate(board_model=33)

    def test_each_chip_has_its_pga_plan_by_default(self):
        self.assertEqual(validate()[meter_calibration.CONF_PGA_CODE], 0x15)
        self.assertEqual(
            meter_calibration.CONFIG_SCHEMA(dict(PM220))[
                meter_calibration.CONF_PGA_CODE
            ],
            0,
        )

    def test_an_atm90e32_takes_any_gain_per_channel(self):
        for code in (0x00, 0x15, 0x2A, 0x24, 0x01):
            with self.subTest(code=code):
                self.assertEqual(
                    validate(pga_code=code)[meter_calibration.CONF_PGA_CODE], code
                )

    def test_an_atm90e32_code_with_a_field_that_is_no_gain_is_refused(self):
        message = (
            "pga_code for atm90e32 is MMode1\\[5:0\\]: three 2-bit fields, "
            "each 0 \\(1X\\), 1 \\(2X\\) or 2 \\(4X\\)"
        )
        for code in (0x03, 0x0C, 0x30, 0x40, 0xFF):
            with self.subTest(code=code), self.assertRaisesRegex(cv.Invalid, message):
                validate(pga_code=code)

    def test_a_bl0906_takes_only_x1(self):
        config = dict(PM220, pga_code=0)
        self.assertEqual(
            meter_calibration.CONFIG_SCHEMA(config)[meter_calibration.CONF_PGA_CODE], 0
        )
        with self.assertRaisesRegex(
            cv.Invalid,
            "pga_code for bl0906 must be 0 \\(x1 on every channel\\), the only plan the record defines",
        ):
            meter_calibration.CONFIG_SCHEMA(dict(PM220, pga_code=1))

    def test_a_pga_code_past_a_byte_is_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "value must be at most 255"):
            validate(pga_code=256)

    def test_the_error_points_at_the_pga_code(self):
        with self.assertRaises(cv.Invalid) as caught:
            validate(pga_code=0x03)
        self.assertEqual(caught.exception.path, [meter_calibration.CONF_PGA_CODE])

    def test_unknown_keys_are_refused(self):
        with self.assertRaisesRegex(cv.Invalid, "file"):
            validate(file="meter.cal")


if __name__ == "__main__":
    unittest.main()
