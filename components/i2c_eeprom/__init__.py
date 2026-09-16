"""Byte access to a 24Cxx-style I2C EEPROM. Vendored from pilotak/esphome-eeprom."""

from esphome import automation
import esphome.codegen as cg
from esphome.components import i2c
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_SIZE

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["i2c"]
MULTI_CONF = True

CONF_ON_SETUP = "on_setup"

# Kilobits, as printed on the part: a 64KB chip holds 8192 bytes.
EEPROM_SIZES = {
    "512KB": 65536,
    "256KB": 32768,
    "128KB": 16384,
    "64KB": 8192,
    "32KB": 4096,
    "16KB": 2048,
    "8KB": 1024,
    "4KB": 512,
    "2KB": 256,
    "1KB": 128,
}

i2c_eeprom_ns = cg.esphome_ns.namespace("i2c_eeprom")
I2CEeprom = i2c_eeprom_ns.class_("I2CEeprom", cg.Component, i2c.I2CDevice)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(I2CEeprom),
            cv.Required(CONF_SIZE): cv.enum(EEPROM_SIZES, upper=True),
            cv.Optional(CONF_ON_SETUP): automation.validate_automation(single=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x50))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_size(config[CONF_SIZE]))
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if CONF_ON_SETUP in config:
        await automation.build_automation(
            var.get_setup_trigger(), [], config[CONF_ON_SETUP]
        )
