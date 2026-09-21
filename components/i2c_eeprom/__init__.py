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
CONF_PAGE_SIZE = "page_size"

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


# The largest page each density in this range writes at once. Naming more than the part has is
# not a bigger page: the chip rolls over inside the transaction and keeps only its tail.
MAX_PAGE_SIZES = {
    "512KB": 128,
    "256KB": 64,
    "128KB": 64,
    "64KB": 32,
    "32KB": 32,
    "16KB": 16,
    "8KB": 16,
    "4KB": 16,
    "2KB": 16,
    "1KB": 16,
}


# The density does not give the page size away: a 2 Kbit AT24C02 pages 8 bytes and an M24C02
# 16. The default is the smallest any 24Cxx uses, so a chunk cut on it lies inside one real
# page whatever the part; naming the part's own page size only saves transactions.
def _page_size(value):
    # A YAML `true` is an int in Python and would pass as a page size of one byte.
    if isinstance(value, bool):
        raise cv.Invalid("page_size must be a number of bytes")
    size = cv.int_range(min=1, max=max(MAX_PAGE_SIZES.values()))(value)
    if size & (size - 1):
        raise cv.Invalid("page_size must be a power of two")
    return size


# Checked here rather than in _page_size, which does not get to see the density.
def _page_fits_the_part(config):
    limit = MAX_PAGE_SIZES[config[CONF_SIZE]]
    if config[CONF_PAGE_SIZE] > limit:
        raise cv.Invalid(
            f"page_size must be at most {limit}, the largest page a {config[CONF_SIZE]} part has",
            path=[CONF_PAGE_SIZE],
        )
    return config


i2c_eeprom_ns = cg.esphome_ns.namespace("i2c_eeprom")
I2CEeprom = i2c_eeprom_ns.class_("I2CEeprom", cg.Component, i2c.I2CDevice)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(I2CEeprom),
            cv.Required(CONF_SIZE): cv.enum(EEPROM_SIZES, upper=True),
            cv.Optional(CONF_PAGE_SIZE, default=8): _page_size,
            cv.Optional(CONF_ON_SETUP): automation.validate_automation(single=True),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(i2c.i2c_device_schema(0x50)),
    _page_fits_the_part,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    cg.add(var.set_size(config[CONF_SIZE]))
    cg.add(var.set_page_size(config[CONF_PAGE_SIZE]))
    await cg.register_component(var, config)
    await i2c.register_i2c_device(var, config)

    if CONF_ON_SETUP in config:
        await automation.build_automation(
            var.get_setup_trigger(), [], config[CONF_ON_SETUP]
        )
