"""The identity in a JetHome CPU board's EEPROM: board, model, serial and the factory signature, as read."""

import esphome.codegen as cg
from esphome.components import i2c_eeprom
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["i2c_eeprom"]

CONF_EEPROM_ID = "eeprom_id"
CONF_PROTECT_EEPROM = "protect_eeprom"

jethome_board_info_ns = cg.esphome_ns.namespace("jethome_board_info")
JetHomeBoardInfo = jethome_board_info_ns.class_("JetHomeBoardInfo", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(JetHomeBoardInfo),
        cv.Required(CONF_EEPROM_ID): cv.use_id(i2c_eeprom.I2CEeprom),
        cv.Optional(CONF_PROTECT_EEPROM, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    eeprom = await cg.get_variable(config[CONF_EEPROM_ID])
    cg.add(var.set_eeprom(eeprom))
    # Generated code runs before App.setup(), so no on_setup automation can write first.
    cg.add(eeprom.set_write_protected(config[CONF_PROTECT_EEPROM]))
