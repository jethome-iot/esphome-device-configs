"""Writes the panic record ESPHome's crash handler kept across the reset to a storage mount.

The first boot after a crash saves it as <report_dir>/crash0.txt behind a header naming the
firmware that wrote it; older reports shift up to crash<keep-1>.txt, the oldest is dropped.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import filesystem_storage_abstract, logger
from esphome.const import CONF_ID, PLATFORM_ESP32, PLATFORM_HOST
from esphome.core import CORE

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["filesystem_storage_abstract", "logger"]

CONF_STORAGE = "storage"
CONF_REPORT_DIR = "report_dir"
CONF_KEEP = "keep"

crash_report_ns = cg.esphome_ns.namespace("crash_report")
CrashReport = crash_report_ns.class_("CrashReport", cg.Component)


def folder_name(value):
    # One folder below the storage: a path would put the files, and the mkdir, elsewhere.
    value = cv.string_strict(value)
    if not value or "/" in value or "\\" in value or value in (".", ".."):
        raise cv.Invalid("report_dir must be a single folder name")
    if ".." in value:
        raise cv.Invalid(
            "report_dir cannot contain '..': web_file_browser rejects such a path"
        )
    return value


def not_under_arduino(config):
    # ESPHome builds no crash handler when Arduino is linked, so there would be nothing to save.
    if CORE.is_esp32 and CORE.using_arduino:
        raise cv.Invalid("crash_report needs the esp-idf framework")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(CrashReport),
            cv.Required(CONF_STORAGE): cv.use_id(
                filesystem_storage_abstract.FilesystemStorageAbstract
            ),
            cv.Optional(CONF_REPORT_DIR, default="crash"): folder_name,
            cv.Optional(CONF_KEEP, default=4): cv.int_range(min=1, max=16),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
    not_under_arduino,
)


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
