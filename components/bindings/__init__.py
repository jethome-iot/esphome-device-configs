"""A binary_sensor drives a switch (toggle or follow); bindings are stored per switch by jxd_config."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]

bindings_ns = cg.esphome_ns.namespace("bindings")
BindingsManager = bindings_ns.class_("BindingsManager", cg.Component)

CONFIG_SCHEMA = cv.Schema({cv.GenerateID(): cv.declare_id(BindingsManager)})


def _final_validate(config):
    full = fv.full_config.get()
    for platform in ("switch", "binary_sensor"):
        if platform not in full:
            raise cv.Invalid(f"bindings needs a '{platform}:' section")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    # Guards the binding fields in jxd_config's per-switch settings.
    cg.add_define("JXD_CONFIG_BINDINGS")
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
