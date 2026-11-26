"""The device's timezone: Home Assistant's, the one compiled in, or one chosen at run time.

Home Assistant's zone is kept across reboots in a restoring global the configuration names; a
zone chosen at run time (web_device_dashboard's /api/device/time) in the component's own
preference. The clocks it is handed report which of them synced last; the clock can also be set
by hand, which fires on_clock_set.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import time as time_
from esphome.components.globals import GlobalsComponent
import esphome.config_validation as cv
from esphome.const import (
    CONF_ID,
    CONF_PLATFORM,
    CONF_RESTORE_VALUE,
    CONF_TIMEZONE,
    CONF_TYPE,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
import esphome.final_validate as fv
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["time"]

CONF_HOMEASSISTANT_ZONE = "homeassistant_zone"
CONF_CLOCKS = "clocks"
CONF_HOMEASSISTANT = "homeassistant"
CONF_SNTP = "sntp"
CONF_RTC = "rtc"
CONF_ON_CLOCK_SET = "on_clock_set"

# What the global must be for the component to point into it.
ZONE_TYPES = (
    "esphome::time::ParsedTimezone",
    "::esphome::time::ParsedTimezone",
    "time::ParsedTimezone",
)

timezone_settings_ns = cg.esphome_ns.namespace("timezone_settings")
TimezoneSettings = timezone_settings_ns.class_("TimezoneSettings", cg.Component)
ClockSource = timezone_settings_ns.enum("ClockSource", is_class=True)

CLOCK_SOURCES = {
    CONF_HOMEASSISTANT: ClockSource.HOMEASSISTANT,
    CONF_SNTP: ClockSource.SNTP,
    CONF_RTC: ClockSource.RTC,
}

# posix_tz_parse.cpp's bounds, either way of zero: POSIX's for an offset, and a rule's time as
# the struct holds it.
MAX_OFFSET = 24 * 3600
MAX_RULE_TIME = 2**31 - 1


def _posix(value):
    """The POSIX string of a validated zone: time's own validator, which reads tzdata."""
    return time_.validate_tz(value)


def validate_zone(value):
    """time's `timezone:`, plus what the device's own parser refuses that Python's takes."""
    value = cv.string_strict(value)
    if not value:
        raise cv.Invalid("timezone must name a zone, such as UTC or Europe/Berlin")
    posix = _posix(value)
    if not all(" " <= c <= "~" for c in posix):
        raise cv.Invalid(
            f"POSIX timezone string '{posix}' must be printable ASCII: "
            "the device's parser reads no other"
        )
    from aioesphomeapi.posix_tz import parse_posix_tz

    parsed = parse_posix_tz(posix)
    offsets = (parsed.std_offset_seconds, parsed.dst_offset_seconds)
    if any(abs(offset) > MAX_OFFSET for offset in offsets):
        raise cv.Invalid(
            f"POSIX timezone string '{posix}' has an offset more than 24 hours from UTC"
        )
    times = (parsed.dst_start.time_seconds, parsed.dst_end.time_seconds)
    if any(abs(seconds) > MAX_RULE_TIME for seconds in times):
        raise cv.Invalid(
            f"POSIX timezone string '{posix}' has a DST rule time past 596523 hours"
        )
    return value


def _ha_zone_needs_ha_clock(config):
    if CONF_HOMEASSISTANT_ZONE in config and CONF_HOMEASSISTANT not in config.get(
        CONF_CLOCKS, {}
    ):
        raise cv.Invalid(
            "homeassistant_zone needs clocks: homeassistant: the zone is taken when that "
            "clock syncs",
            path=[CONF_HOMEASSISTANT_ZONE],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TimezoneSettings),
            cv.Required(CONF_TIMEZONE): validate_zone,
            cv.Optional(CONF_HOMEASSISTANT_ZONE): cv.use_id(GlobalsComponent),
            cv.Optional(CONF_CLOCKS): cv.Schema(
                {
                    cv.Optional(key): cv.use_id(time_.RealTimeClock)
                    for key in CLOCK_SOURCES
                }
            ),
            # One automation: the trigger holds one.
            cv.Optional(CONF_ON_CLOCK_SET): automation.validate_automation(single=True),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
    _ha_zone_needs_ha_clock,
)


def _find(full_config, domain, ident):
    for entry in full_config.get(domain) or []:
        if entry[CONF_ID].id == ident:
            return entry
    return None


def _final_validate(config):
    full_config = fv.full_config.get()
    if CONF_HOMEASSISTANT_ZONE in config:
        ident = config[CONF_HOMEASSISTANT_ZONE].id
        entry = _find(full_config, "globals", ident)
        if (
            entry is None
            or str(entry[CONF_TYPE]).strip() not in ZONE_TYPES
            or not entry[CONF_RESTORE_VALUE]
        ):
            raise cv.Invalid(
                f"homeassistant_zone: {ident} must be a global of type "
                "esphome::time::ParsedTimezone with restore_value: true",
                path=[CONF_HOMEASSISTANT_ZONE],
            )
    clock = config.get(CONF_CLOCKS, {}).get(CONF_HOMEASSISTANT)
    if clock is not None:
        entry = _find(full_config, "time", clock.id)
        if entry is not None and CONF_TIMEZONE in entry:
            raise cv.Invalid(
                f"clocks: homeassistant: {clock.id} has a timezone of its own; Home "
                f"Assistant sends its zone only to a {entry[CONF_PLATFORM]} time without one",
                path=[CONF_CLOCKS, CONF_HOMEASSISTANT],
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    # posix_tz.cpp builds only with it, and the component calls into it.
    cg.add_define("USE_TIME_TIMEZONE")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_default_zone(config[CONF_TIMEZONE], _posix(config[CONF_TIMEZONE])))
    cg.add(var.set_preference_hash(fnv1_hash(config[CONF_ID].id)))
    if CONF_HOMEASSISTANT_ZONE in config:
        zone = await cg.get_variable(config[CONF_HOMEASSISTANT_ZONE])
        cg.add(var.set_homeassistant_zone(cg.RawExpression(f"&{zone}->value()")))
    for key, clock_id in config.get(CONF_CLOCKS, {}).items():
        clock = await cg.get_variable(clock_id)
        cg.add(var.add_clock(clock, CLOCK_SOURCES[key]))
    if CONF_ON_CLOCK_SET in config:
        await automation.build_automation(
            var.get_clock_set_trigger(), [], config[CONF_ON_CLOCK_SET]
        )
