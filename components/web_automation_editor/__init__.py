"""JSON API for the automations component at <url_prefix>/api/*; the dashboard's editor is its client."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.automations import AutomationStorage
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["web_server_base", "automations"]

CONF_AUTOMATIONS_ID = "automations_id"
CONF_URL_PREFIX = "url_prefix"

web_automation_editor_ns = cg.esphome_ns.namespace("web_automation_editor")
WebAutomationEditor = web_automation_editor_ns.class_(
    "WebAutomationEditor", cg.Component
)


def url_prefix(value):
    # The routes hang off "<prefix>/api/", so the prefix is one leading slash and no trailing one.
    value = cv.string_strict(value).strip("/")
    if not value:
        raise cv.Invalid("url_prefix must name a path below the server root")
    if any(c.isspace() or c in "?#" for c in value):
        raise cv.Invalid("url_prefix must be a URL path: no spaces, '?' or '#'")
    return "/" + value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WebAutomationEditor),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.GenerateID(CONF_AUTOMATIONS_ID): cv.use_id(AutomationStorage),
        cv.Optional(CONF_URL_PREFIX, default="/automation-editor"): url_prefix,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    web_base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    storage = await cg.get_variable(config[CONF_AUTOMATIONS_ID])
    var = cg.new_Pvariable(config[CONF_ID], web_base, storage)
    cg.add(var.set_url_prefix(config[CONF_URL_PREFIX]))
    await cg.register_component(var, config)
