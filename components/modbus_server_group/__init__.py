"""Modbus Server Group Component

This component automatically configures a Modbus server with coils and discrete inputs
based on ESPHome groups. It eliminates the need for manual lambda configuration by
dynamically creating server registers from group entities.

Features:
- Auto-generates server coils from output group (switches)
- Auto-generates server discrete inputs from input group (binary sensors)
- Provides registers at 0x0200 (inputs count) and 0x0201 (outputs count)
"""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import modbus_controller
from esphome.components.groups import GroupClass
from esphome.components.modbus_controller.const import CONF_MODBUS_CONTROLLER_ID
from esphome.const import CONF_ID
from esphome.core import CORE

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["modbus_controller", "groups"]

CONF_INPUTS_GROUP = "inputs_group"
CONF_INPUTS_START_ADDRESS = "inputs_start_address"
CONF_OUTPUTS_GROUP = "outputs_group"
CONF_OUTPUTS_START_ADDRESS = "outputs_start_address"

modbus_server_group_ns = cg.esphome_ns.namespace("modbus_server_group")
ModbusServerGroup = modbus_server_group_ns.class_(
    "ModbusServerGroup", cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ModbusServerGroup),
        cv.Required(CONF_MODBUS_CONTROLLER_ID): cv.use_id(
            modbus_controller.ModbusController
        ),
        cv.Optional(CONF_INPUTS_GROUP): cv.use_id(GroupClass),
        cv.Optional(CONF_INPUTS_START_ADDRESS, default=0xA100): cv.hex_int_range(
            min=0x0000, max=0xFFFF
        ),
        cv.Optional(CONF_OUTPUTS_GROUP): cv.use_id(GroupClass),
        cv.Optional(CONF_OUTPUTS_START_ADDRESS, default=0xA000): cv.hex_int_range(
            min=0x0000, max=0xFFFF
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def validate_at_least_one_group(config):
    """Validate that at least one group is configured."""
    if CONF_INPUTS_GROUP not in config and CONF_OUTPUTS_GROUP not in config:
        raise cv.Invalid(
            f"At least one of '{CONF_INPUTS_GROUP}' or '{CONF_OUTPUTS_GROUP}' must be specified"
        )
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, validate_at_least_one_group)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # Get modbus controller reference
    modbus_controller_var = await cg.get_variable(config[CONF_MODBUS_CONTROLLER_ID])
    cg.add(var.set_modbus_controller(modbus_controller_var))

    # Configure inputs group if specified
    if CONF_INPUTS_GROUP in config:
        inputs_group_var = await cg.get_variable(config[CONF_INPUTS_GROUP])
        cg.add(var.set_inputs_group(inputs_group_var))
        cg.add(var.set_inputs_start_address(config[CONF_INPUTS_START_ADDRESS]))

    # Configure outputs group if specified
    if CONF_OUTPUTS_GROUP in config:
        outputs_group_var = await cg.get_variable(config[CONF_OUTPUTS_GROUP])
        cg.add(var.set_outputs_group(outputs_group_var))
        cg.add(var.set_outputs_start_address(config[CONF_OUTPUTS_START_ADDRESS]))

