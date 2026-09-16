"""Host stand-in for upstream's web_server_base: no network, no socket; the tests drive the handlers."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

web_server_base_ns = cg.esphome_ns.namespace("web_server_base")
WebServerBase = web_server_base_ns.class_("WebServerBase")

CONF_WEB_SERVER_BASE_ID = "web_server_base_id"

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(WebServerBase)})


async def to_code(config):
    cg.new_Pvariable(config[CONF_ID])
