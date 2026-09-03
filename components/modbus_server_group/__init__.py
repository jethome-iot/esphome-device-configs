"""Modbus Server Group Component

This component automatically configures a Modbus RTU server with coils and discrete
inputs based on ESPHome groups. It eliminates the need for manual lambda configuration
by dynamically creating server bits from group entities.

It is a `modbus::ModbusServerDevice` in its own right and attaches directly to a
`modbus:` hub configured with `role: server`. Unlike the upstream `modbus_server`
component, coils (FC 0x01/0x05/0x0F) and discrete inputs (FC 0x02) are kept in two
independent address spaces, so the same address can carry a relay coil and a digital
input at once.

Features:
- Auto-generates server coils from the outputs group (switches)
- Auto-generates server discrete inputs from the inputs group (binary sensors)
- Provides input registers at 0x0200 (inputs count) and 0x0201 (outputs count)
- Optional courtesy response for unmapped addresses
"""

import esphome.codegen as cg
from esphome.components import modbus
from esphome.components.groups import GroupClass
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["modbus", "groups"]

CONF_INPUTS_GROUP = "inputs_group"
CONF_INPUTS_START_ADDRESS = "inputs_start_address"
CONF_OUTPUTS_GROUP = "outputs_group"
CONF_OUTPUTS_START_ADDRESS = "outputs_start_address"

CONF_COURTESY_RESPONSE = "courtesy_response"
CONF_ENABLED = "enabled"
CONF_REGISTER_LAST_ADDRESS = "register_last_address"
CONF_REGISTER_VALUE = "register_value"
CONF_COIL_LAST_ADDRESS = "coil_last_address"
CONF_COIL_VALUE = "coil_value"
CONF_DISCRETE_INPUT_LAST_ADDRESS = "discrete_input_last_address"
CONF_DISCRETE_INPUT_VALUE = "discrete_input_value"

modbus_server_group_ns = cg.esphome_ns.namespace("modbus_server_group")
ModbusServerGroup = modbus_server_group_ns.class_(
    "ModbusServerGroup", cg.Component, modbus.ModbusServerDevice
)
CourtesyResponse = modbus_server_group_ns.struct("CourtesyResponse")

COURTESY_RESPONSE_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ENABLED, default=False): cv.boolean,
        cv.Optional(CONF_REGISTER_LAST_ADDRESS, default=0xFFFF): cv.hex_uint16_t,
        cv.Optional(CONF_REGISTER_VALUE, default=0): cv.hex_uint16_t,
        cv.Optional(CONF_COIL_LAST_ADDRESS, default=0xFFFF): cv.hex_uint16_t,
        cv.Optional(CONF_COIL_VALUE, default=False): cv.boolean,
        cv.Optional(CONF_DISCRETE_INPUT_LAST_ADDRESS, default=0xFFFF): cv.hex_uint16_t,
        cv.Optional(CONF_DISCRETE_INPUT_VALUE, default=False): cv.boolean,
    }
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ModbusServerGroup),
            cv.Optional(CONF_COURTESY_RESPONSE): COURTESY_RESPONSE_SCHEMA,
            cv.Optional(CONF_INPUTS_GROUP): cv.use_id(GroupClass),
            cv.Optional(CONF_INPUTS_START_ADDRESS, default=0xA100): cv.hex_int_range(
                min=0x0000, max=0xFFFF
            ),
            cv.Optional(CONF_OUTPUTS_GROUP): cv.use_id(GroupClass),
            cv.Optional(CONF_OUTPUTS_START_ADDRESS, default=0xA000): cv.hex_int_range(
                min=0x0000, max=0xFFFF
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(modbus.modbus_device_schema(0x01, role="server"))
)


def validate_at_least_one_group(config):
    """Validate that at least one group is configured."""
    if CONF_INPUTS_GROUP not in config and CONF_OUTPUTS_GROUP not in config:
        raise cv.Invalid(
            f"At least one of '{CONF_INPUTS_GROUP}' or '{CONF_OUTPUTS_GROUP}' must be specified"
        )
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, validate_at_least_one_group)


def _final_validate(config):
    return modbus.final_validate_modbus_device("modbus_server_group", role="server")(
        config
    )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if courtesy := config.get(CONF_COURTESY_RESPONSE):
        cg.add(
            var.set_courtesy_response(
                cg.StructInitializer(
                    CourtesyResponse,
                    ("enabled", courtesy[CONF_ENABLED]),
                    ("register_last_address", courtesy[CONF_REGISTER_LAST_ADDRESS]),
                    ("register_value", courtesy[CONF_REGISTER_VALUE]),
                    ("coil_last_address", courtesy[CONF_COIL_LAST_ADDRESS]),
                    ("coil_value", courtesy[CONF_COIL_VALUE]),
                    (
                        "discrete_input_last_address",
                        courtesy[CONF_DISCRETE_INPUT_LAST_ADDRESS],
                    ),
                    ("discrete_input_value", courtesy[CONF_DISCRETE_INPUT_VALUE]),
                )
            )
        )

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

    await modbus.register_modbus_server_device(var, config)
