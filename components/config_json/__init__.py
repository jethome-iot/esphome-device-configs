"""Keeps settings types as <config_dir>/<key>.json files on a filesystem_storage_abstract mount."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import filesystem_storage_abstract
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["filesystem_storage_abstract"]
AUTO_LOAD = ["json", "config_base"]

CONF_STORAGE = "storage"
CONF_CONFIG_DIR = "config_dir"
CONF_SAVE_DELAY = "save_delay"

config_json_ns = cg.esphome_ns.namespace("config_json")
ConfigJsonKeeper = config_json_ns.class_("ConfigJsonKeeper", cg.Component)
SettingsBaseJson = config_json_ns.class_("SettingsBaseJson")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ConfigJsonKeeper),
        cv.Required(CONF_STORAGE): cv.use_id(
            filesystem_storage_abstract.FilesystemStorageAbstract
        ),
        cv.Optional(CONF_CONFIG_DIR, default="config"): cv.string,
        cv.Optional(
            CONF_SAVE_DELAY, default="10s"
        ): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    cg.add_define("USE_CONFIG_JSON")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CORE.is_esp32:
        from esphome.components import esp32

        esp32.require_vfs_dir()  # mkdir on the config directory
    storage = await cg.get_variable(config[CONF_STORAGE])
    cg.add(var.set_storage(storage))
    cg.add(var.set_config_dir(config[CONF_CONFIG_DIR]))
    cg.add(var.set_save_delay(config[CONF_SAVE_DELAY]))
