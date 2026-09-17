"""The device dashboard at /, plus its device API under /api/device/."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["web_server_base", "web_server"]

CONF_BOARD_INFO_ID = "board_info_id"

web_device_dashboard_ns = cg.esphome_ns.namespace("web_device_dashboard")
WebDeviceDashboard = web_device_dashboard_ns.class_("WebDeviceDashboard", cg.Component)
# Named, not imported: the component is optional, and a config without it has nothing to import.
JetHomeBoardInfo = cg.esphome_ns.namespace("jethome_board_info").class_(
    "JetHomeBoardInfo", cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WebDeviceDashboard),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.Optional(CONF_BOARD_INFO_ID): cv.use_id(JetHomeBoardInfo),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    web_base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], web_base)
    if CONF_BOARD_INFO_ID in config:
        cg.add_define("USE_WEB_DEVICE_DASHBOARD_BOARD_INFO")
        board = await cg.get_variable(config[CONF_BOARD_INFO_ID])
        cg.add(var.set_board_info(board))
    await cg.register_component(var, config)
