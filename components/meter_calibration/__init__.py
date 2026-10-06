"""The factory calibration record a metering board keeps in its own EEPROM, read and checked at boot."""

import esphome.codegen as cg
from esphome.components import jethome_board_info
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["jethome_board_info"]

CONF_BOARD_INFO_ID = "board_info_id"
CONF_CHIP = "chip"
CONF_BOARD_MODEL = "board_model"
CONF_PGA_CODE = "pga_code"

# The record's board_model field.
BOARD_MODEL_SIZE = 32

meter_calibration_ns = cg.esphome_ns.namespace("meter_calibration")
MeterCalibration = meter_calibration_ns.class_("MeterCalibration", cg.Component)
ChipType = meter_calibration_ns.enum("ChipType", is_class=True)

CHIPS = {
    "atm90e32": ChipType.ATM90E32,
    "bl0906": ChipType.BL0906,
}

# The PGA plan each board is calibrated at: PGA 2X on every ATM90E32 current channel, x1 on
# every BL0906 channel. A record taken at another setting is refused.
DEFAULT_PGA_CODES = {
    "atm90e32": 0x15,
    "bl0906": 0x00,
}


def _board_model(value):
    value = cv.string_strict(value)
    if not value:
        raise cv.Invalid("board_model must not be empty")
    if len(value) > BOARD_MODEL_SIZE:
        raise cv.Invalid(
            f"board_model is at most {BOARD_MODEL_SIZE} characters, the record's field"
        )
    if not all(" " <= c <= "~" for c in value):
        raise cv.Invalid("board_model must be printable ASCII")
    return value


# Checked here rather than in the key's own validator, which does not get to see the chip.
def _pga_code_fits_the_chip(config):
    chip = config[CONF_CHIP]
    if CONF_PGA_CODE not in config:
        config[CONF_PGA_CODE] = DEFAULT_PGA_CODES[chip]
        return config
    code = config[CONF_PGA_CODE]
    if chip == "atm90e32":
        # MMode1[5:0]: a 2-bit gain per current channel, 3 is not a gain.
        fields = [(code >> shift) & 0x3 for shift in (0, 2, 4)]
        if code > 0x3F or 3 in fields:
            raise cv.Invalid(
                "pga_code for atm90e32 is MMode1[5:0]: three 2-bit fields, each 0 (1X), 1 (2X) or 2 (4X)",
                path=[CONF_PGA_CODE],
            )
    elif code != 0:
        raise cv.Invalid(
            "pga_code for bl0906 must be 0 (x1 on every channel), the only plan the record defines",
            path=[CONF_PGA_CODE],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MeterCalibration),
            cv.Required(CONF_BOARD_INFO_ID): cv.use_id(
                jethome_board_info.JetHomeBoardInfo
            ),
            cv.Required(CONF_CHIP): cv.one_of(*CHIPS, lower=True),
            cv.Required(CONF_BOARD_MODEL): _board_model,
            cv.Optional(CONF_PGA_CODE): cv.hex_uint8_t,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _pga_code_fits_the_chip,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    board_info = await cg.get_variable(config[CONF_BOARD_INFO_ID])
    cg.add(var.set_board_info(board_info))
    cg.add(var.set_chip(CHIPS[config[CONF_CHIP]]))
    cg.add(var.set_board_model(config[CONF_BOARD_MODEL]))
    cg.add(var.set_pga_code(config[CONF_PGA_CODE]))
