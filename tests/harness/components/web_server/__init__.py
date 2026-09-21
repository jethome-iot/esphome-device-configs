"""Host stand-in for upstream's web_server, which builds for ESP platforms only.

web_device_dashboard depends on it for the entity REST API its page calls, not for anything
this suite drives, so the stand-in only carries what the dependency and the entity platforms
ask of it: the per-entity sorting block, which nothing here declares, and the `auth:` block,
which decides the defines and the compiled-in credentials web_auth replaces.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_AUTH, CONF_PASSWORD, CONF_TYPE, CONF_USERNAME

# As upstream's: the dashboard builds its answers with json::build_json.
AUTO_LOAD = ["json"]

AUTH_TYPE_BASIC = "basic"
AUTH_TYPE_DIGEST = "digest"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.Optional(CONF_AUTH): cv.Schema(
            {
                cv.Required(CONF_USERNAME): cv.All(cv.string_strict, cv.Length(min=1)),
                cv.Required(CONF_PASSWORD): cv.All(cv.string_strict, cv.Length(min=1)),
                cv.Optional(CONF_TYPE, default=AUTH_TYPE_DIGEST): cv.one_of(
                    AUTH_TYPE_BASIC, AUTH_TYPE_DIGEST, lower=True
                ),
            }
        ),
    }
)

WEBSERVER_SORTING_SCHEMA = cv.Schema({})


async def to_code(config):
    if (auth := config.get(CONF_AUTH)) is None:
        return
    # Upstream's ESP32 and digest path: the pair goes in as two pointers to generated
    # literals, which is the one web_auth overrides.
    cg.add_define("USE_WEBSERVER_AUTH")
    if auth[CONF_TYPE] == AUTH_TYPE_DIGEST:
        cg.add_define("USE_WEBSERVER_AUTH_DIGEST")
    paren = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(paren.set_auth_username(auth[CONF_USERNAME]))
    cg.add(paren.set_auth_password(auth[CONF_PASSWORD]))


async def add_entity_config(entity, config):
    pass
