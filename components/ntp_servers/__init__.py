"""The NTP servers the device asks: up to three set at run time, then the SNTP clock's own.

The servers set at run time (web_device_dashboard's /api/device/time) are kept in the
component's preference; the clock's own are the fallback, which can be turned off.
"""

import esphome.codegen as cg
from esphome.components import time as time_
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_PLATFORM,
    CONF_SERVERS,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import CORE
import esphome.final_validate as fv
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["time"]

CONF_SNTP_ID = "sntp_id"

ntp_servers_ns = cg.esphome_ns.namespace("ntp_servers")
NTPServers = ntp_servers_ns.class_("NTPServers", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(NTPServers),
            cv.Required(CONF_SNTP_ID): cv.use_id(time_.RealTimeClock),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)


def _clock(full_config, ident):
    for entry in full_config.get("time") or []:
        if entry[CONF_ID].id == ident:
            return entry
    return None


def _final_validate(config):
    ident = config[CONF_SNTP_ID].id
    entry = _clock(fv.full_config.get(), ident)
    # The host has no sntp platform; its suite drives a host clock.
    if entry is not None and entry[CONF_PLATFORM] != "sntp" and not CORE.is_host:
        raise cv.Invalid(
            f"sntp_id: {ident} is a {entry[CONF_PLATFORM]} time; the servers are the sntp "
            "platform's",
            path=[CONF_SNTP_ID],
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_preference_hash(fnv1_hash(config[CONF_ID].id)))
    clock = await cg.get_variable(config[CONF_SNTP_ID])
    cg.add(var.set_clock(clock))
    entry = _clock(CORE.config, config[CONF_SNTP_ID].id)
    for server in (entry or {}).get(CONF_SERVERS, []):
        cg.add(var.add_fallback_server(server))
