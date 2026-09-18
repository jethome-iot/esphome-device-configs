"""The update entity: the device manifest on one side, ota.http_request on the other."""

import esphome.codegen as cg
from esphome.components import ota, update
from esphome.components.http_request import CONF_HTTP_REQUEST_ID, HttpRequestComponent
from esphome.components.http_request.ota import OtaHttpRequestComponent
import esphome.config_validation as cv
from esphome.const import CONF_CHANNEL, CONF_SOURCE

from . import jethome_update_ns

AUTO_LOAD = ["jethome_manifest"]
DEPENDENCIES = ["ota.http_request"]

CONF_OTA_ID = "ota_id"

JethomeUpdate = jethome_update_ns.class_(
    "JethomeUpdate", update.UpdateEntity, cg.PollingComponent
)


def _channel(value):
    """The channel is matched against the manifest's slot names, so it has to be a bare word."""
    value = cv.string_strict(value)
    if value.split() != [value]:
        raise cv.Invalid("Channel must be a non-empty name without spaces")
    return value


CONFIG_SCHEMA = (
    update.update_schema(JethomeUpdate)
    .extend(
        {
            cv.GenerateID(CONF_OTA_ID): cv.use_id(OtaHttpRequestComponent),
            cv.GenerateID(CONF_HTTP_REQUEST_ID): cv.use_id(HttpRequestComponent),
            cv.Required(CONF_SOURCE): cv.url,
            cv.Optional(CONF_CHANNEL, default="release"): _channel,
        }
    )
    .extend(cv.polling_component_schema("6h"))
)


async def to_code(config):
    var = await update.new_update(config)
    await cg.register_component(var, config)

    ota_parent = await cg.get_variable(config[CONF_OTA_ID])
    cg.add(var.set_ota_parent(ota_parent))
    request_parent = await cg.get_variable(config[CONF_HTTP_REQUEST_ID])
    cg.add(var.set_request_parent(request_parent))

    cg.add(var.set_source_url(config[CONF_SOURCE]))
    cg.add(var.set_channel(config[CONF_CHANNEL]))

    # The install progress and its failures come from the OTA component.
    ota.request_ota_state_listeners()
