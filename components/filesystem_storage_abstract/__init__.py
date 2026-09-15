"""Abstract filesystem storage interface; concrete backends (littlefs_storage) inherit from it."""

import esphome.codegen as cg
import esphome.config_validation as cv

CODEOWNERS = ["@jethome-iot"]

filesystem_storage_abstract_ns = cg.esphome_ns.namespace("filesystem_storage_abstract")
FilesystemStorageAbstract = filesystem_storage_abstract_ns.class_(
    "FilesystemStorageAbstract", cg.Component
)

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
