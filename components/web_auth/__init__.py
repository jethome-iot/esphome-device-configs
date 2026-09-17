"""The HTTP credentials web_server checks, replaceable at runtime and kept in flash."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import (
    CONF_AUTH,
    CONF_ID,
    CONF_PASSWORD,
    CONF_TYPE,
    CONF_USERNAME,
    CONF_WEB_SERVER,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import CORE
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["web_server_base", "web_server"]

AUTH_TYPE_DIGEST = "digest"

web_auth_ns = cg.esphome_ns.namespace("web_auth")
WebAuth = web_auth_ns.class_("WebAuth", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(WebAuth),
            cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
                web_server_base.WebServerBase
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)


def _final_validate(config):
    auth = (fv.full_config.get().get(CONF_WEB_SERVER) or {}).get(CONF_AUTH)
    if auth is None:
        raise cv.Invalid(
            "web_auth needs a web_server with an 'auth:' block: the authentication "
            "middleware is compiled in only when the credentials are set at build time, "
            "so without one there is nothing to replace"
        )
    if auth.get(CONF_TYPE) != AUTH_TYPE_DIGEST and not CORE.is_esp32:
        raise cv.Invalid(
            "web_auth needs 'auth:' with 'type: digest' off ESP32: a basic-auth build "
            "there checks a hash computed at build time, which no runtime credential "
            "can replace"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    cg.add_define("USE_WEB_AUTH")

    web_base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], web_base)
    await cg.register_component(var, config)
    cg.add(var.set_preference_hash(fnv1_hash(config[CONF_ID].id)))
    # The compiled auth: block is the factory default: what a device with nothing stored
    # serves, and what the dashboard calls out as unchanged.
    auth = CORE.config[CONF_WEB_SERVER][CONF_AUTH]
    cg.add(var.set_default_credentials(auth[CONF_USERNAME], auth[CONF_PASSWORD]))
