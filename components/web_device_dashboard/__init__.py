"""The device dashboard at /, plus its device API under /api/device/."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID, PLATFORM_ESP32, PLATFORM_HOST
from esphome.core import CORE

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["web_server_base", "web_server"]
AUTO_LOAD = ["web_origin_guard"]

CONF_BOARD_INFO_ID = "board_info_id"
CONF_STORAGE_ID = "storage_id"
CONF_URL_PREFIX = "url_prefix"

# The components whose screens the page draws, and the setter each one's prefix goes to.
# Read off the config rather than wired in by hand: they are configured in packages of their
# own, and a device may include any of them without the others.
SERVED_BY = {
    "web_file_browser": "set_files_url_prefix",
    "web_automation_editor": "set_automations_url_prefix",
}

web_device_dashboard_ns = cg.esphome_ns.namespace("web_device_dashboard")
WebDeviceDashboard = web_device_dashboard_ns.class_("WebDeviceDashboard", cg.Component)
# Named, not imported: the components are optional, and a config without them has nothing to
# import.
JetHomeBoardInfo = cg.esphome_ns.namespace("jethome_board_info").class_(
    "JetHomeBoardInfo", cg.Component
)
FilesystemStorageAbstract = cg.esphome_ns.namespace(
    "filesystem_storage_abstract"
).class_("FilesystemStorageAbstract", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebDeviceDashboard),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
            cv.Optional(CONF_BOARD_INFO_ID): cv.use_id(JetHomeBoardInfo),
            cv.Optional(CONF_STORAGE_ID): cv.use_id(FilesystemStorageAbstract),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)


async def to_code(config):
    web_base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], web_base)
    if CONF_BOARD_INFO_ID in config:
        cg.add_define("USE_WEB_DEVICE_DASHBOARD_BOARD_INFO")
        board = await cg.get_variable(config[CONF_BOARD_INFO_ID])
        cg.add(var.set_board_info(board))
    if CONF_STORAGE_ID in config:
        cg.add_define("USE_WEB_DEVICE_DASHBOARD_STORAGE")
        storage = await cg.get_variable(config[CONF_STORAGE_ID])
        cg.add(var.set_storage(storage))
    for component, setter in SERVED_BY.items():
        served = CORE.config.get(component)
        if served is not None:
            cg.add(getattr(var, setter)(served[CONF_URL_PREFIX]))
    await cg.register_component(var, config)
