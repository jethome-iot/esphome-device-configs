"""MQTT topics read into entities: slots set from the dashboard, one entity per enabled slot.

The slots are a JSON file on a filesystem storage and their entities are created at boot, so
codegen reserves the entities' places in App's tables and registers the units they may carry.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import filesystem_storage_abstract, mqtt_config, web_server
from esphome.const import (
    CONF_ID,
    CONF_WEB_SERVER,
    CONF_WEB_SERVER_ID,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import CORE
from esphome.core.entity_helpers import register_unit_of_measurement

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["mqtt", "mqtt_config", "filesystem_storage_abstract"]
AUTO_LOAD = ["sensor", "binary_sensor", "text_sensor", "json"]

CONF_MQTT_CONFIG_ID = "mqtt_config_id"
CONF_STORAGE = "storage"
CONF_FOLDER_PATH = "folder_path"
CONF_MAX_SLOTS = "max_slots"
CONF_UNITS = "units"
CONF_DALLAS_SCAN = "dallas_scan"

MAX_SLOTS = 16
MAX_UNITS = 32
UNIT_MAX_BYTES = 16
DEFAULT_UNITS = [
    "°C",
    "°F",
    "%",
    "W",
    "kW",
    "Wh",
    "kWh",
    "V",
    "A",
    "Hz",
    "lx",
    "ppm",
    "µg/m³",
    "hPa",
    "Pa",
    "bar",
    "m³",
    "L",
    "L/min",
    "m³/h",
    "s",
    "min",
    "h",
    "dB",
    "dBm",
    "mm",
    "m/s",
]
# Every slot can be any of them, so each domain gets max_slots places.
DOMAINS = ("sensor", "binary_sensor", "text_sensor")
# Components that keep a folder of their own on a storage, and the option naming it. The
# automations engine loads every JSON file in its folder as a rule; crash_report prunes its.
FOLDER_OWNERS = {
    "automations": "folder_path",
    "crash_report": "report_dir",
    "config_json": "config_dir",
}

mqtt_subscriptions_ns = cg.esphome_ns.namespace("mqtt_subscriptions")
MqttSubscriptions = mqtt_subscriptions_ns.class_("MqttSubscriptions", cg.Component)


def folder_name(value):
    # One folder below the storage: a path would put the file, and the mkdir, elsewhere.
    value = cv.string_strict(value)
    if not value or "/" in value or "\\" in value or value in (".", ".."):
        raise cv.Invalid("folder_path must be a single folder name")
    return value


def unit_list(value):
    units = cv.ensure_list(cv.string_strict)(value)
    if len(units) > MAX_UNITS:
        raise cv.Invalid(f"at most {MAX_UNITS} units")
    seen = set()
    for unit in units:
        if not 1 <= len(unit.encode("utf-8")) <= UNIT_MAX_BYTES:
            raise cv.Invalid(f"unit '{unit}' must be 1 to {UNIT_MAX_BYTES} bytes")
        if unit in seen:
            raise cv.Invalid(f"unit '{unit}' is listed twice")
        seen.add(unit)
    return units


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MqttSubscriptions),
            cv.GenerateID(CONF_MQTT_CONFIG_ID): cv.use_id(mqtt_config.MqttConfig),
            cv.Required(CONF_STORAGE): cv.use_id(
                filesystem_storage_abstract.FilesystemStorageAbstract
            ),
            cv.Optional(CONF_FOLDER_PATH, default="mqtt"): folder_name,
            cv.Optional(CONF_MAX_SLOTS, default=8): cv.int_range(min=1, max=MAX_SLOTS),
            cv.Optional(CONF_UNITS, default=DEFAULT_UNITS): unit_list,
        }
    )
    .extend(web_server.WEBSERVER_SORTING_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)


def shared_folder(config, full_config):
    """The component whose folder on the same storage `folder_path` names, or None."""
    for domain, key in FOLDER_OWNERS.items():
        other = full_config.get(domain)
        if (
            isinstance(other, dict)
            and other.get(CONF_STORAGE) == config[CONF_STORAGE]
            and other.get(key) == config[CONF_FOLDER_PATH]
        ):
            return domain
    return None


def _final_validate(config):
    if (owner := shared_folder(config, fv.full_config.get())) is not None:
        raise cv.Invalid(
            f"folder_path '{config[CONF_FOLDER_PATH]}' is {owner}'s folder on the same "
            "storage: pick another",
            path=[CONF_FOLDER_PATH],
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def probe_names(full_config):
    """(name_prefix, max_sensors) of dallas_scan, whose Temp N come after the slots; None without it."""
    if (scan := full_config.get(CONF_DALLAS_SCAN)) is None:
        return None
    return scan["name_prefix"], scan["max_sensors"]


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    # The entities are registered at run time: reserve their places in App's tables before any
    # await, so the count is in before the platform defines are written.
    for _ in range(config[CONF_MAX_SLOTS]):
        for domain in DOMAINS:
            CORE.register_platform_component(domain, var)
    cg.add_define("USE_MQTT_SUBSCRIPTIONS")
    cg.add_define("USE_ENTITY_UNIT_OF_MEASUREMENT")
    await cg.register_component(var, config)

    if CORE.is_esp32:
        from esphome.components import esp32

        esp32.require_vfs_dir()  # mkdir on the slots' folder
    cg.add(var.set_config(await cg.get_variable(config[CONF_MQTT_CONFIG_ID])))
    cg.add(var.set_storage(await cg.get_variable(config[CONF_STORAGE])))
    cg.add(var.set_folder_path(config[CONF_FOLDER_PATH]))
    cg.add(var.set_max_slots(config[CONF_MAX_SLOTS]))
    for unit in config[CONF_UNITS]:
        cg.add(var.add_unit(unit, register_unit_of_measurement(unit)))
    if (names := probe_names(CORE.config)) is not None:
        cg.add(var.reserve_sensor_names(*names))

    # Same group hash as web_server.add_entity_config() computes for YAML entities.
    if sorting := config.get(CONF_WEB_SERVER):
        server = await cg.get_variable(sorting[CONF_WEB_SERVER_ID])
        group = hash(sorting.get(web_server.CONF_SORTING_GROUP_ID))
        weight = sorting.get(web_server.CONF_SORTING_WEIGHT, 50)
        cg.add_define("USE_WEBSERVER_SORTING")
        cg.add(var.set_web_server_sorting(server, group, weight))
