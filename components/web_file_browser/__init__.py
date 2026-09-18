"""JSON file API at <url_prefix>/* over a filesystem_storage_abstract mount; the dashboard's Files screen is its client."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import CONF_ID, CONF_OTA, CONF_PLATFORM, CONF_WEB_SERVER
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.components import filesystem_storage_abstract
from esphome.core import CORE

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["web_server_base"]
AUTO_LOAD = ["filesystem_storage_abstract"]

CONF_STORAGE_ID = "storage_id"
CONF_URL_PREFIX = "url_prefix"


def url_prefix(value):
    # Validated here, not in to_code: web_device_dashboard reads it off the config to report
    # the Files screen in /api/device/capabilities, and a client builds URLs from it.
    value = cv.string_strict(value).strip("/")
    if not value:
        raise cv.Invalid("url_prefix must name a path below the server root")
    if any(c.isspace() or c in "?#" for c in value):
        raise cv.Invalid("url_prefix must be a URL path: no spaces, '?' or '#'")
    # A browser resolves these away before it sends the request and the handler matches the
    # prefix literally, so a route named with one could never be reached.
    if any(segment in ("", ".", "..") for segment in value.split("/")):
        raise cv.Invalid("url_prefix must not contain empty or dot path segments")
    return "/" + value


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
        cv.Optional(CONF_URL_PREFIX, default="/files"): url_prefix,
    }
).extend(cv.COMPONENT_SCHEMA)


def _validate_multipart_available(config):
    # On ESP32 the multipart body reader web_server_idf needs for /upload is
    # compiled in by USE_WEBSERVER_OTA, and only the web_server OTA platform
    # defines it. Without it uploads are answered "No file received" and lost.
    if not CORE.is_esp32:
        return config
    for platform in fv.full_config.get().get(CONF_OTA) or []:
        if platform.get(CONF_PLATFORM) == CONF_WEB_SERVER:
            return config
    raise cv.Invalid(
        "web_file_browser needs the multipart body reader that "
        f"'{CONF_OTA}: - {CONF_PLATFORM}: {CONF_WEB_SERVER}' compiles in "
        "(it defines USE_WEBSERVER_OTA); without it uploads are silently dropped"
    )


FINAL_VALIDATE_SCHEMA = _validate_multipart_available


async def to_code(config):
    web_base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    storage = await cg.get_variable(config[CONF_STORAGE_ID])

    if CORE.is_esp32:
        from esphome.components import esp32

        esp32.require_vfs_dir()  # opendir/readdir/mkdir/rmdir behind the API
    var = cg.new_Pvariable(config[CONF_ID], web_base, storage)
    cg.add(var.set_url_prefix(config[CONF_URL_PREFIX]))
    await cg.register_component(var, config)
