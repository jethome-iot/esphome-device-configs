"""JSON file API at <url_prefix>/* over a filesystem_storage_abstract mount; the dashboard's Files screen is its client."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.components import filesystem_storage_abstract
from esphome.core import CORE

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["web_server_base"]
AUTO_LOAD = ["filesystem_storage_abstract"]

CONF_STORAGE_ID = "storage_id"
CONF_URL_PREFIX = "url_prefix"

web_file_browser_ns = cg.esphome_ns.namespace("web_file_browser")
WebFileBrowser = web_file_browser_ns.class_("WebFileBrowser", cg.Component)


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WebFileBrowser),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.Required(CONF_STORAGE_ID): cv.use_id(
            filesystem_storage_abstract.FilesystemStorageAbstract
        ),
        cv.Optional(CONF_URL_PREFIX, default="/files"): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    web_base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    storage = await cg.get_variable(config[CONF_STORAGE_ID])

    # Normalize url_prefix to ensure it starts with /
    url_prefix = config[CONF_URL_PREFIX]
    if not url_prefix.startswith("/"):
        url_prefix = "/" + url_prefix

    if CORE.is_esp32:
        from esphome.components import esp32

        esp32.require_vfs_dir()  # opendir/readdir/mkdir/rmdir behind the API
    var = cg.new_Pvariable(config[CONF_ID], web_base, storage)
    cg.add(var.set_url_prefix(url_prefix))
    await cg.register_component(var, config)
