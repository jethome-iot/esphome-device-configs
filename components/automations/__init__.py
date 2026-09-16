"""Runtime automations: rules stored as JSON files on a filesystem storage, built at boot and editable at run time."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import filesystem_storage_abstract
from esphome.core import CORE
from esphome.components import time as time_
from esphome.const import CONF_ID, CONF_TIME_ID

CODEOWNERS = ["@jethome-iot"]
# time: the cron path holds a RealTimeClock and includes its header even when
# time_id is left out, so the component is always compiled in.
DEPENDENCIES = ["filesystem_storage_abstract"]
AUTO_LOAD = ["json", "time"]

CONF_STORAGE = "storage"
CONF_FOLDER_PATH = "folder_path"


def folder_name(value):
    # One folder below the storage: a path would let the engine scan and delete elsewhere.
    value = cv.string_strict(value)
    if not value or "/" in value or "\\" in value or value in (".", ".."):
        raise cv.Invalid("folder_path must be a single folder name")
    return value


automations_ns = cg.esphome_ns.namespace("automations")
AutomationStorage = automations_ns.class_("AutomationStorage", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AutomationStorage),
        cv.Required(CONF_STORAGE): cv.use_id(
            filesystem_storage_abstract.FilesystemStorageAbstract
        ),
        cv.Optional(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        cv.Optional(CONF_FOLDER_PATH, default="automations"): folder_name,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CORE.is_esp32:
        from esphome.components import esp32

        esp32.require_vfs_dir()  # readdir/mkdir on the automations folder
    storage = await cg.get_variable(config[CONF_STORAGE])
    cg.add(var.set_storage(storage))
    cg.add(var.set_folder_path(config[CONF_FOLDER_PATH]))
    if CONF_TIME_ID in config:
        rtc = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_source(rtc))
