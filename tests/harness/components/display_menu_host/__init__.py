"""Test-only key that pulls display_menu_base into a host build: it has none of its own.

With an id it also declares a menu that draws nothing, which the display_menu actions can name.
"""

import esphome.codegen as cg
from esphome.components.display_menu_base import DisplayMenuComponent
import esphome.config_validation as cv
from esphome.const import CONF_ID

AUTO_LOAD = ["display_menu_base"]

display_menu_host_ns = cg.esphome_ns.namespace("display_menu_host")
HostMenu = display_menu_host_ns.class_("HostMenu", DisplayMenuComponent)

CONFIG_SCHEMA = cv.Schema({cv.Optional(CONF_ID): cv.declare_id(HostMenu)})


async def to_code(config):
    if CONF_ID in config:
        cg.new_Pvariable(config[CONF_ID])
