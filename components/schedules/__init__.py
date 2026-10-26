"""Weekly schedules for relays and climate_hub thermostats, stored as JSON files on a filesystem storage."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import filesystem_storage_abstract
from esphome.components import time as time_
from esphome.const import CONF_ID, CONF_TIME_ID
from esphome.core import CORE

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["filesystem_storage_abstract", "time"]
# climate_hub stays optional: without it a thermostat target is reported missing.
AUTO_LOAD = ["json", "loop_job", "switch_hold"]

CONF_STORAGE = "storage"
CONF_FOLDER_PATH = "folder_path"
CONF_MAX_SCHEDULES = "max_schedules"

# The components that keep a folder of their own on a storage, and its default.
FOLDER_OWNERS = (("climate_hub", "climates"), ("automations", "automations"))


def folder_name(value):
    # One folder below the storage: a path would let the engine scan and delete elsewhere.
    value = cv.string_strict(value)
    if not value or "/" in value or "\\" in value or value in (".", ".."):
        raise cv.Invalid("folder_path must be a single folder name")
    return value


schedules_ns = cg.esphome_ns.namespace("schedules")
ScheduleEngine = schedules_ns.class_("ScheduleEngine", cg.Component)


def _not_the_namespace(config):
    # The generated global would make `schedules::` ambiguous in main.cpp.
    if config[CONF_ID].id == "schedules":
        raise cv.Invalid(
            "id 'schedules' is the component's C++ namespace; name it schedules_engine",
            path=[CONF_ID],
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ScheduleEngine),
            cv.Required(CONF_STORAGE): cv.use_id(
                filesystem_storage_abstract.FilesystemStorageAbstract
            ),
            cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
            cv.Optional(CONF_FOLDER_PATH, default="schedules"): folder_name,
            cv.Optional(CONF_MAX_SCHEDULES, default=16): cv.int_range(min=1, max=32),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _not_the_namespace,
)


def _final_validate(config):
    # Each loads every *.json in its folder and would take the other's files for broken ones.
    full = fv.full_config.get()
    for owner, default in FOLDER_OWNERS:
        other = full.get(owner)
        if (
            not isinstance(other, dict)
            or other.get(CONF_STORAGE) != config[CONF_STORAGE]
        ):
            continue
        folder = config[CONF_FOLDER_PATH]
        if other.get(CONF_FOLDER_PATH, default) == folder:
            raise cv.Invalid(
                f"folder_path '{folder}' is {owner}'s folder", path=[CONF_FOLDER_PATH]
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add_define("USE_SCHEDULES")

    if CORE.is_esp32:
        from esphome.components import esp32

        esp32.require_vfs_dir()  # readdir/mkdir on the schedules folder
    storage = await cg.get_variable(config[CONF_STORAGE])
    cg.add(var.set_storage(storage))
    rtc = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time_source(rtc))
    cg.add(var.set_folder_path(config[CONF_FOLDER_PATH]))
    cg.add(var.set_max_schedules(config[CONF_MAX_SCHEDULES]))
