"""LittleFS on a dedicated flash partition (ESP-IDF only); the partition is added to the table here."""

import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32, filesystem_storage_abstract
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["filesystem_storage_abstract"]

CONF_PARTITION_LABEL = "partition_label"
CONF_PARTITION_SIZE = "partition_size"
CONF_BASE_PATH = "base_path"
CONF_FORMAT_IF_MOUNT_FAILED = "format_if_mount_failed"

LITTLEFS_COMPONENT_VERSION = "1.22.3"

littlefs_storage_ns = cg.esphome_ns.namespace("littlefs_storage")
LittleFSStorage = littlefs_storage_ns.class_(
    "LittleFSStorage", filesystem_storage_abstract.FilesystemStorageAbstract
)


# Binary units: 4MB is 4 MiB, unlike cv.validate_bytes.
def _partition_size(value):
    if isinstance(value, str):
        match = re.fullmatch(r"\s*(\d+)\s*([KM]?)B?\s*", value, re.IGNORECASE)
        if match is None:
            raise cv.Invalid("Expected a size like 4MB, 512KB or a number of bytes")
        value = (
            int(match.group(1))
            * {"": 1, "K": 1024, "M": 1024 * 1024}[match.group(2).upper()]
        )
    size = cv.int_range(min=0x1000)(value)
    if size % 0x1000 != 0:
        raise cv.Invalid("partition_size must be a multiple of 4KB")
    return size


CONFIG_SCHEMA = cv.All(
    cv.only_on_esp32,
    cv.only_with_framework(cv.Framework.ESP_IDF),
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LittleFSStorage),
            cv.Optional(CONF_PARTITION_LABEL, default="littlefs"): cv.string_strict,
            cv.Optional(CONF_PARTITION_SIZE, default="1MB"): _partition_size,
            cv.Optional(CONF_BASE_PATH, default="/littlefs"): cv.string,
            cv.Optional(CONF_FORMAT_IF_MOUNT_FAILED, default=True): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    esp32.add_idf_component(name="joltwallet/littlefs", ref=LITTLEFS_COMPONENT_VERSION)
    esp32.add_partition(
        config[CONF_PARTITION_LABEL], "data", "littlefs", config[CONF_PARTITION_SIZE]
    )

    cg.add(var.set_partition_label(config[CONF_PARTITION_LABEL]))
    cg.add(var.set_base_path(config[CONF_BASE_PATH]))
    cg.add(var.set_format_if_mount_failed(config[CONF_FORMAT_IF_MOUNT_FAILED]))
