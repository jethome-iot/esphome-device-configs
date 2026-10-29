"""Binary sensors that follow a physical input through an on and an off delay, set up at run time."""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import web_server
from esphome.const import (
    CONF_ID,
    CONF_INCLUDE_INTERNAL,
    CONF_NAME,
    CONF_WEB_SERVER,
    CONF_WEB_SERVER_ID,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import CORE
from esphome.helpers import sanitize, snake_case

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["config_json"]
AUTO_LOAD = ["binary_sensor", "json"]

CONF_MAX_INPUTS = "max_inputs"
CONF_CONFIG_JSON_ID = "config_json_id"

# What C++ names slot N; its object id is what rules and bindings keep.
NAME_PREFIX = "Virtual input"

virtual_inputs_ns = cg.esphome_ns.namespace("virtual_inputs")
VirtualInputs = virtual_inputs_ns.class_("VirtualInputs", cg.Component)
# Named here rather than imported, as dallas_scan does.
ConfigJsonKeeper = cg.esphome_ns.namespace("config_json").class_(
    "ConfigJsonKeeper", cg.Component
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(VirtualInputs),
            cv.GenerateID(CONF_CONFIG_JSON_ID): cv.use_id(ConfigJsonKeeper),
            # Both the slots and the binary sensors codegen reserves.
            cv.Optional(CONF_MAX_INPUTS, default=16): cv.int_range(min=1, max=32),
        }
    )
    .extend(web_server.WEBSERVER_SORTING_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)


def slot_object_id(slot):
    """The object id of slot N, counted from 1."""
    return sanitize(snake_case(f"{NAME_PREFIX} {slot}"))


def _final_validate(config):
    full = fv.full_config.get()
    server = full.get("web_server")
    if isinstance(server, dict) and server.get(CONF_INCLUDE_INTERNAL):
        raise cv.Invalid(
            "virtual_inputs keeps its free slots internal; "
            "web_server include_internal: true would list them"
        )
    # The slots are registered at run time, past upstream's duplicate check.
    taken = {
        slot_object_id(slot): slot for slot in range(1, config[CONF_MAX_INPUTS] + 1)
    }
    for entry in full.get("binary_sensor") or []:
        name = entry.get(CONF_NAME)
        if isinstance(name, str) and (slot := taken.get(sanitize(snake_case(name)))):
            raise cv.Invalid(
                f"binary_sensor '{name}' has the object id of {NAME_PREFIX} {slot}; "
                "give it another name"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    # The slots are registered at run time: reserve their places in App's table before any
    # await, so the count is in before the platform defines are written.
    for _ in range(config[CONF_MAX_INPUTS]):
        CORE.register_platform_component("binary_sensor", var)
    await cg.register_component(var, config)
    cg.add(var.set_max_inputs(config[CONF_MAX_INPUTS]))
    cg.add(var.set_keeper(await cg.get_variable(config[CONF_CONFIG_JSON_ID])))

    # The labels are entity_config's binary_sensor records, when the firmware keeps them.
    entity_config = CORE.config.get("entity_config")
    if entity_config is not None and "binary_sensor" in entity_config["settings"]:
        settings = await cg.get_variable(entity_config["binary_sensor_settings_id"])
        cg.add(var.set_input_settings(settings))
    # The relays' bindings, which a delete drops, when the firmware has them.
    if (
        entity_config is not None
        and "switch" in entity_config["settings"]
        and "bindings" in CORE.config
    ):
        relays = await cg.get_variable(entity_config["switch_settings_id"])
        cg.add(var.set_switch_settings(relays))
    # The rules that name a slot, and their rebuild when it comes or goes, when the firmware runs
    # rules: a lambda keeps their engine's header out of this component.
    if (automations := CORE.config.get("automations")) is not None:
        engine = await cg.get_variable(automations[CONF_ID])
        cg.add(
            var.set_rule_finder(
                cg.RawExpression(
                    f"[](uint32_t key) {{ return {engine}->rules_naming_binary_sensor(key); }}"
                )
            )
        )
        cg.add(
            var.set_rule_rebuilder(
                cg.RawExpression(
                    f"[](uint32_t key) {{ {engine}->rebuild_rules_naming_binary_sensor(key); }}"
                )
            )
        )

    # Same group hash as web_server.add_entity_config() computes for YAML entities.
    sorting = config.get(CONF_WEB_SERVER)
    if sorting and CONF_WEB_SERVER_ID in sorting:
        server = await cg.get_variable(sorting[CONF_WEB_SERVER_ID])
        group = hash(sorting.get(web_server.CONF_SORTING_GROUP_ID))
        weight = sorting.get(web_server.CONF_SORTING_WEIGHT, 50)
        cg.add_define("USE_WEBSERVER_SORTING")
        cg.add(var.set_web_server_sorting(server, group, weight))
