"""The one_wire platform of the harness bus; upstream's gpio one needs ISR pins the host lacks."""

import esphome.codegen as cg
from esphome.components.one_wire import OneWireBus
import esphome.config_validation as cv
from esphome.const import CONF_ID

one_wire_host_ns = cg.esphome_ns.namespace("one_wire_host")
HostOneWireBus = one_wire_host_ns.class_("HostOneWireBus", OneWireBus, cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HostOneWireBus),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
