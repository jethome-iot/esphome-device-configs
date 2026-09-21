"""Origin/Host guard for the shared web server; auto-loaded by its users, no block to write."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["web_server_base"]

web_origin_guard_ns = cg.esphome_ns.namespace("web_origin_guard")
CrossOriginRefuser = web_origin_guard_ns.class_("CrossOriginRefuser", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CrossOriginRefuser),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    web_base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], web_base)
    await cg.register_component(var, config)
