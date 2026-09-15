"""Test-only storage backend: a directory on the host filesystem."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import filesystem_storage_abstract
from esphome.const import CONF_ID, CONF_PATH

AUTO_LOAD = ["filesystem_storage_abstract"]

dir_storage_ns = cg.esphome_ns.namespace("dir_storage")
DirStorage = dir_storage_ns.class_(
    "DirStorage", filesystem_storage_abstract.FilesystemStorageAbstract
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(DirStorage),
        cv.Required(CONF_PATH): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_base_path(config[CONF_PATH]))
