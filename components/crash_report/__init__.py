"""Prototype: persists ESPHome's panic record to a file so a field device can hand it back.

Feasibility spike for issue #63. The panic-time half already exists upstream
(esphome/components/esp32/crash_handler.cpp, --wrap=esp_panic_handler); this only copies
the record out at the next boot, where the scheduler and VFS are up.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import filesystem_storage_abstract, logger
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["filesystem_storage_abstract", "logger", "esp32"]

CONF_STORAGE = "storage"
CONF_REPORT_DIR = "report_dir"
CONF_KEEP = "keep"

crash_report_ns = cg.esphome_ns.namespace("crash_report")
CrashReport = crash_report_ns.class_("CrashReport", cg.Component)


def folder_name(value):
    value = cv.string_strict(value)
    if not value or "/" in value or "\\" in value or value in (".", ".."):
        raise cv.Invalid("report_dir must be a single folder name")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(CrashReport),
        cv.Required(CONF_STORAGE): cv.use_id(
            filesystem_storage_abstract.FilesystemStorageAbstract
        ),
        cv.Optional(CONF_REPORT_DIR, default="crash"): folder_name,
        cv.Optional(CONF_KEEP, default=4): cv.int_range(min=1, max=16),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    logger.request_log_listener()  # our own slot; the count sizes logger's StaticVector

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CORE.is_esp32:
        from esphome.components import esp32

        esp32.require_vfs_dir()  # mkdir on the report directory

    storage = await cg.get_variable(config[CONF_STORAGE])
    cg.add(var.set_storage(storage))
    cg.add(var.set_report_dir(config[CONF_REPORT_DIR]))
    cg.add(var.set_keep(config[CONF_KEEP]))
