"""Thermostats stored as JSON files on a filesystem storage and run by the firmware, editable at run time."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import filesystem_storage_abstract, web_server
from esphome.const import (
    CONF_ID,
    CONF_INCLUDE_INTERNAL,
    CONF_WEB_SERVER,
    CONF_WEB_SERVER_ID,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import CORE
from esphome.core.entity_helpers import register_icon

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["filesystem_storage_abstract"]
# No sensor or switch: the C++ finds nothing without them, as automations does.
AUTO_LOAD = ["climate", "json", "loop_job", "switch_hold"]

CONF_STORAGE = "storage"
CONF_FOLDER_PATH = "folder_path"
CONF_MAX_CONTROLLERS = "max_controllers"

ICON = "mdi:thermostat"


def folder_name(value):
    # One folder below the storage: a path would let the hub scan and write elsewhere.
    value = cv.string_strict(value)
    if not value or "/" in value or "\\" in value or value in (".", ".."):
        raise cv.Invalid("folder_path must be a single folder name")
    return value


climate_hub_ns = cg.esphome_ns.namespace("climate_hub")
ClimateHub = climate_hub_ns.class_("ClimateHub", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ClimateHub),
            cv.Required(CONF_STORAGE): cv.use_id(
                filesystem_storage_abstract.FilesystemStorageAbstract
            ),
            cv.Optional(CONF_FOLDER_PATH, default="climates"): folder_name,
            # Both the documents and the climate entities codegen reserves.
            cv.Optional(CONF_MAX_CONTROLLERS, default=8): cv.int_range(min=1, max=16),
        }
    )
    .extend(web_server.WEBSERVER_SORTING_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)


def _final_validate(config):
    server = fv.full_config.get().get("web_server")
    if isinstance(server, dict) and server.get(CONF_INCLUDE_INTERNAL):
        raise cv.Invalid(
            "climate_hub keeps its unused thermostat entities internal; "
            "web_server include_internal: true would list them"
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    # The entities are registered at run time: reserve their places in App's table before
    # any await, so the count is in before the platform defines are written.
    for _ in range(config[CONF_MAX_CONTROLLERS]):
        CORE.register_platform_component("climate", var)
    cg.add_define("USE_CLIMATE_HUB")
    await cg.register_component(var, config)

    if CORE.is_esp32:
        from esphome.components import esp32

        esp32.require_vfs_dir()  # readdir/mkdir on the thermostats folder
    storage = await cg.get_variable(config[CONF_STORAGE])
    cg.add(var.set_storage(storage))
    cg.add(var.set_folder_path(config[CONF_FOLDER_PATH]))
    cg.add(var.set_max_controllers(config[CONF_MAX_CONTROLLERS]))
    cg.add_define("USE_ENTITY_ICON")
    cg.add(var.set_icon_index(register_icon(ICON)))

    # Same group hash as web_server.add_entity_config() computes for YAML entities.
    sorting = config.get(CONF_WEB_SERVER)
    if sorting and CONF_WEB_SERVER_ID in sorting:
        server = await cg.get_variable(sorting[CONF_WEB_SERVER_ID])
        group = hash(sorting.get(web_server.CONF_SORTING_GROUP_ID))
        weight = sorting.get(web_server.CONF_SORTING_WEIGHT, 50)
        cg.add_define("USE_WEBSERVER_SORTING")
        cg.add(var.set_web_server_sorting(server, group, weight))
