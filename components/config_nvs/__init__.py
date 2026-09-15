"""Keeps settings types in NVS namespaces; for values that must survive a filesystem wipe."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
AUTO_LOAD = ["config_base"]

CONF_SAVE_DELAY = "save_delay"

config_nvs_ns = cg.esphome_ns.namespace("config_nvs")
ConfigNvsKeeper = config_nvs_ns.class_("ConfigNvsKeeper", cg.Component)
SettingsBaseNvs = config_nvs_ns.class_("SettingsBaseNvs")

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ConfigNvsKeeper),
            cv.Optional(
                CONF_SAVE_DELAY, default="5s"
            ): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on_esp32,
)


async def to_code(config):
    cg.add_define("USE_CONFIG_NVS")

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_save_delay(config[CONF_SAVE_DELAY]))
