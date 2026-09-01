"""A display with no bus behind it, served over HTTP as a front panel page.

Swap it in for the physical driver where the bus does not exist — under QEMU,
which emulates no I2C controller — and the pages, fonts and menus render
unchanged, because they are written against the generic display API.
"""

import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, display, web_server_base
from esphome.components.web_server_base import CONF_WEB_SERVER_BASE_ID
from esphome.const import CONF_HEIGHT, CONF_ID, CONF_LAMBDA, CONF_WIDTH

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["web_server_base"]
AUTO_LOAD = ["binary_sensor"]

CONF_URL_PREFIX = "url_prefix"
CONF_KEYS = "keys"
CONF_HOLD_TIME = "hold_time"

virtual_display_ns = cg.esphome_ns.namespace("virtual_display")
VirtualDisplay = virtual_display_ns.class_(
    "VirtualDisplay", cg.PollingComponent, display.DisplayBuffer
)


_KEY_NAME_RE = re.compile(r"^[A-Za-z0-9_-]+$")


def _validate_key_name(value):
    # A key name is a URL path segment; keep it to characters that survive a URL
    # untouched, so the posted path is the name whatever the client encodes.
    value = cv.string_strict(value)
    if not _KEY_NAME_RE.match(value):
        raise cv.Invalid(
            f"Key name {value!r} must be non-empty and use only letters, digits, '_' or '-'"
        )
    return value


def _validate_url_prefix(value):
    value = cv.string_strict(value)
    if not value.startswith("/"):
        raise cv.Invalid("url_prefix must start with '/'")
    if len(value) < 2 or value.endswith("/"):
        # A trailing slash would make the sub-paths (/info, /frame, /key/...)
        # unreachable, and the component would look dead but for its page.
        raise cv.Invalid("url_prefix must not be '/' or end with '/'")
    return value


CONFIG_SCHEMA = display.FULL_DISPLAY_SCHEMA.extend(
    {
        cv.GenerateID(): cv.declare_id(VirtualDisplay),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.Optional(CONF_WIDTH, default=128): cv.int_range(min=8, max=1024),
        cv.Optional(CONF_HEIGHT, default=64): cv.int_range(min=8, max=1024),
        cv.Optional(CONF_URL_PREFIX, default="/panel"): _validate_url_prefix,
        # How long an injected press stays down when the caller does not manage
        # down/up itself. Long enough to survive a debounce filter, short enough
        # not to trip autorepeat.
        cv.Optional(CONF_HOLD_TIME, default="120ms"): cv.positive_time_period_milliseconds,
        # Name -> the binary_sensor the front panel publishes to. The sensor has
        # to be one nothing else drives every loop (a `template` one), or the
        # injected state is overwritten before any automation sees it.
        cv.Optional(CONF_KEYS): cv.Schema(
            {_validate_key_name: cv.use_id(binary_sensor.BinarySensor)}
        ),
    }
).extend(cv.polling_component_schema("200ms"))


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await display.register_display(var, config)

    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    cg.add(var.set_base(base))
    cg.add(var.set_dimensions(config[CONF_WIDTH], config[CONF_HEIGHT]))
    cg.add(var.set_url_prefix(config[CONF_URL_PREFIX]))
    cg.add(var.set_hold_time(config[CONF_HOLD_TIME].total_milliseconds))

    for name, key_id in config.get(CONF_KEYS, {}).items():
        cg.add(var.add_key(name, await cg.get_variable(key_id)))

    if CONF_LAMBDA in config:
        lambda_ = await cg.process_lambda(
            config[CONF_LAMBDA], [(display.DisplayRef, "it")], return_type=cg.void
        )
        cg.add(var.set_writer(lambda_))
