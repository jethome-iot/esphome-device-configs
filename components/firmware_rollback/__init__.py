"""Boots the firmware in the other app slot: whether there is one to go back to, and the switch.

The instance holds the other slot as last read and drives a rollback from YAML: the
firmware_rollback.refresh and firmware_rollback.rollback actions and the
firmware_rollback.is_available condition. web_device_dashboard auto-loads it and calls the
free functions for its rollback route; a display menu declares it with an id for its rows.
"""

from esphome import automation
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_ON_ERROR, PLATFORM_ESP32, PLATFORM_HOST

CODEOWNERS = ["@jethome-iot"]

firmware_rollback_ns = cg.esphome_ns.namespace("firmware_rollback")
FirmwareRollback = firmware_rollback_ns.class_("FirmwareRollback", cg.Component)
RefreshAction = firmware_rollback_ns.class_("RefreshAction", automation.Action)
RollbackAction = firmware_rollback_ns.class_("RollbackAction", automation.Action)
IsAvailableCondition = firmware_rollback_ns.class_(
    "IsAvailableCondition", automation.Condition
)

CONFIG_SCHEMA = cv.All(
    cv.Schema({cv.GenerateID(): cv.declare_id(FirmwareRollback)}).extend(
        cv.COMPONENT_SCHEMA
    ),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)

FIRMWARE_ROLLBACK_ID_SCHEMA = automation.maybe_simple_id(
    {cv.GenerateID(): cv.use_id(FirmwareRollback)}
)

ROLLBACK_ACTION_SCHEMA = automation.maybe_simple_id(
    {
        cv.GenerateID(): cv.use_id(FirmwareRollback),
        cv.Optional(CONF_ON_ERROR): automation.validate_automation(single=True),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)


@automation.register_action(
    "firmware_rollback.refresh",
    RefreshAction,
    FIRMWARE_ROLLBACK_ID_SCHEMA,
    synchronous=True,
)
async def refresh_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var


# Synchronous: it either reboots or fires on_error before it returns.
@automation.register_action(
    "firmware_rollback.rollback",
    RollbackAction,
    ROLLBACK_ACTION_SCHEMA,
    synchronous=True,
)
async def rollback_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    if error_conf := config.get(CONF_ON_ERROR):
        await automation.build_automation(
            var.get_error_trigger(), [(cg.std_string, "x")], error_conf
        )
    return var


@automation.register_condition(
    "firmware_rollback.is_available",
    IsAvailableCondition,
    FIRMWARE_ROLLBACK_ID_SCHEMA,
)
async def is_available_condition_to_code(config, condition_id, template_arg, args):
    var = cg.new_Pvariable(condition_id, template_arg)
    await cg.register_parented(var, config[CONF_ID])
    return var
