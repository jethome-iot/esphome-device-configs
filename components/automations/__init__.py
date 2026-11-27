"""Runtime automations: rules stored as JSON files on a filesystem storage, built at boot and editable at run time."""

import logging

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import filesystem_storage_abstract
from esphome.core import CORE
from esphome.components import time as time_
from esphome.const import CONF_ID, CONF_TIME_ID
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@jethome-iot"]
# time: the cron path holds a RealTimeClock and includes its header even when
# time_id is left out, so the component is always compiled in.
DEPENDENCIES = ["filesystem_storage_abstract"]
AUTO_LOAD = ["case_fold", "json", "time", "loop_job", "switch_hold"]

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


# Rules nest up to 8 events deep on the loop task, and on ESP32 web_server builds each state
# event for an /events client on that stack too: ESPHome's default 8192 B overflow (#345).
MIN_LOOP_TASK_STACK_SIZE = 12288
_LOGGER = logging.getLogger(__name__)


def _validate_loop_task_stack(config):
    if not CORE.is_esp32:
        return config
    full = fv.full_config.get()
    if "web_server" not in full:
        return config
    advanced = full.get("esp32", {}).get("framework", {}).get("advanced", {})
    size = advanced.get("loop_task_stack_size", 8192)
    if size < MIN_LOOP_TASK_STACK_SIZE:
        _LOGGER.warning(
            "automations with web_server need 'esp32: framework: advanced: "
            "loop_task_stack_size: %d' or more: rules nested 8 deep overflow a %d B loop task "
            "stack while a client holds /events open",
            MIN_LOOP_TASK_STACK_SIZE,
            size,
        )
    return config


FINAL_VALIDATE_SCHEMA = _validate_loop_task_stack


async def to_code(config):
    cg.add_define(
        "USE_AUTOMATIONS"
    )  # entity_config tells the engine about re-emitted levels
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
        cg.add(var.set_preference_hash(fnv1_hash(config[CONF_ID].id)))
