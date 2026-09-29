"""JSON API for the climate_hub thermostats at <url_prefix>/api/*; the dashboard's thermostat editor is its client."""

import re

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import web_server_base
from esphome.components.climate_hub import ClimateHub
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["web_server_base", "climate_hub"]
AUTO_LOAD = ["web_origin_guard", "loop_job"]

CONF_CLIMATE_HUB_ID = "climate_hub_id"
CONF_URL_PREFIX = "url_prefix"

web_climate_editor_ns = cg.esphome_ns.namespace("web_climate_editor")
WebClimateEditor = web_climate_editor_ns.class_("WebClimateEditor", cg.Component)


# What a browser leaves alone in a path segment.
_SEGMENT = re.compile(r"[A-Za-z0-9._~-]+")

# The first path segments upstream's web_server answers itself: its entity domains, the event
# stream and the files of its own page.
WEB_SERVER_PATHS = frozenset(
    {
        "alarm_control_panel",
        "binary_sensor",
        "button",
        "climate",
        "cover",
        "date",
        "datetime",
        "event",
        "events",
        "fan",
        "infrared",
        "light",
        "lock",
        "number",
        "radio_frequency",
        "select",
        "sensor",
        "switch",
        "text",
        "text_sensor",
        "time",
        "update",
        "valve",
        "water_heater",
        "0.css",
        "0.js",
    }
)

# Where web_device_dashboard serves its own API.
DASHBOARD_API = ("api", "device")

# The other handlers whose routes lie below a url_prefix of their own.
PREFIXED_COMPONENTS = ("web_file_browser", "web_automation_editor")


def url_prefix(value):
    # The routes hang off "<prefix>/api/", so the prefix is one leading slash and no trailing one.
    value = cv.string_strict(value).strip("/")
    if not value:
        raise cv.Invalid("url_prefix must name a path below the server root")
    # A browser resolves dot segments, folds backslashes into slashes and percent-encodes the
    # rest before sending; the handler matches literally, so a rewritten prefix is unreachable.
    segments = value.split("/")
    for segment in segments:
        if segment in ("", ".", ".."):
            raise cv.Invalid("url_prefix must not contain empty or dot path segments")
        if not _SEGMENT.fullmatch(segment):
            raise cv.Invalid(
                "url_prefix must be a URL path a browser sends unchanged: "
                "letters, digits, '-', '.', '_' and '~'"
            )
    # The editor claims everything below its prefix, and it sets up at web_server's priority:
    # which of the two answered would come down to registration order.
    if segments[0] in WEB_SERVER_PATHS:
        raise cv.Invalid(
            f"url_prefix must not start with '/{segments[0]}': web_server answers that path"
        )
    if _overlaps(segments, DASHBOARD_API):
        raise cv.Invalid(
            "url_prefix must stay clear of /api/device, which the device dashboard serves"
        )
    return "/" + value


def _overlaps(one, other):
    """Whether one path is the other or lies below it."""
    shorter = min(len(one), len(other))
    return tuple(one[:shorter]) == tuple(other[:shorter])


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(WebClimateEditor),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.GenerateID(CONF_CLIMATE_HUB_ID): cv.use_id(ClimateHub),
        cv.Optional(CONF_URL_PREFIX, default="/climate-editor"): url_prefix,
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config):
    ours = config[CONF_URL_PREFIX].strip("/").split("/")
    full = fv.full_config.get()
    for component in PREFIXED_COMPONENTS:
        other = full.get(component)
        if not isinstance(other, dict) or CONF_URL_PREFIX not in other:
            continue
        if _overlaps(ours, other[CONF_URL_PREFIX].strip("/").split("/")):
            raise cv.Invalid(
                f"url_prefix '{config[CONF_URL_PREFIX]}' overlaps {component}'s "
                f"'{other[CONF_URL_PREFIX]}': both claim what lies below it"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    web_base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    hub = await cg.get_variable(config[CONF_CLIMATE_HUB_ID])
    var = cg.new_Pvariable(config[CONF_ID], web_base, hub)
    cg.add(var.set_url_prefix(config[CONF_URL_PREFIX]))
    await cg.register_component(var, config)
