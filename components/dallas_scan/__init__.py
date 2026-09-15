"""DS18B20 sensors created at boot, one per device on a 1-Wire bus; slot numbers stick in flash."""

import esphome.codegen as cg
from esphome.components import one_wire, sensor, web_server
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_FILTERS,
    CONF_ID,
    CONF_RESOLUTION,
    CONF_SENSOR,
    CONF_WEB_SERVER,
    CONF_WEB_SERVER_ID,
    DEVICE_CLASS_TEMPERATURE,
    UNIT_CELSIUS,
)
from esphome.core import CORE, ID
from esphome.core.entity_helpers import (
    register_device_class,
    register_unit_of_measurement,
)
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["one_wire"]
AUTO_LOAD = ["sensor"]

CONF_MAX_SENSORS = "max_sensors"
CONF_NAME_PREFIX = "name_prefix"
CONF_SLOTS = "slots"

dallas_scan_ns = cg.esphome_ns.namespace("dallas_scan")
DallasScan = dallas_scan_ns.class_("DallasScan", cg.PollingComponent)

SLOT_SCHEMA = cv.Schema(
    {
        cv.Optional(CONF_ADDRESS): cv.hex_uint64_t,
        cv.Optional(CONF_SENSOR): cv.use_id(sensor.Sensor),
    }
)


def _slot(value):
    """A ROM address pinned to the slot, or a mapping with it and/or a YAML sensor."""
    if not isinstance(value, dict):
        value = {CONF_ADDRESS: value}
    value = SLOT_SCHEMA(value)
    if not value:
        raise cv.Invalid(f"A slot needs an {CONF_ADDRESS} or a {CONF_SENSOR}")
    return value


def _fresh_ids(node):
    """A copy of a validated config whose declared ids are new, unnamed ones."""
    if isinstance(node, ID):
        return (
            ID(None, is_declaration=True, type=node.type)
            if node.is_declaration
            else node
        )
    if isinstance(node, dict):
        return {key: _fresh_ids(value) for key, value in node.items()}
    if isinstance(node, list):
        return [_fresh_ids(value) for value in node]
    return node


def _validate(config):
    for slot in config[CONF_SLOTS]:
        if slot > config[CONF_MAX_SENSORS]:
            raise cv.Invalid(
                f"Slot {slot} is above {CONF_MAX_SENSORS} ({config[CONF_MAX_SENSORS]})",
                path=[CONF_SLOTS, slot],
            )
    # A filter chain belongs to one sensor, so every slot gets its own copy; the
    # id pass names the copies' ids after this.
    if (filters := config.get(CONF_FILTERS)) is not None:
        config[CONF_FILTERS] = [filters] + [
            _fresh_ids(filters) for _ in range(config[CONF_MAX_SENSORS] - 1)
        ]
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DallasScan),
            cv.GenerateID(one_wire.CONF_ONE_WIRE_ID): cv.use_id(one_wire.OneWireBus),
            cv.Optional(CONF_MAX_SENSORS, default=8): cv.int_range(min=1, max=64),
            cv.Optional(CONF_NAME_PREFIX, default="Temp"): cv.string_strict,
            cv.Optional(CONF_RESOLUTION, default=12): cv.int_range(min=9, max=12),
            # Slot number -> the address pinned to it and/or the YAML sensor serving it.
            cv.Optional(CONF_SLOTS, default={}): cv.Schema(
                {cv.int_range(min=1, max=64): _slot}
            ),
            # The usual sensor filters, the same chain on every sensor.
            cv.Optional(CONF_FILTERS): sensor.validate_filters,
        }
    )
    .extend(web_server.WEBSERVER_SORTING_SCHEMA)
    .extend(cv.polling_component_schema("60s")),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(
        var.set_one_wire_bus(await cg.get_variable(config[one_wire.CONF_ONE_WIRE_ID]))
    )
    cg.add(var.set_max_sensors(config[CONF_MAX_SENSORS]))
    cg.add(var.set_name_prefix(config[CONF_NAME_PREFIX]))
    cg.add(var.set_resolution(config[CONF_RESOLUTION]))
    cg.add(var.set_preference_hash(fnv1_hash(config[CONF_ID].id)))
    for slot, conf in config[CONF_SLOTS].items():
        if (address := conf.get(CONF_ADDRESS)) is not None:
            cg.add(var.pin(slot - 1, address))
        if (sensor_id := conf.get(CONF_SENSOR)) is not None:
            cg.add(var.set_sensor(slot - 1, await cg.get_variable(sensor_id)))
    for slot, filters in enumerate(config.get(CONF_FILTERS) or []):
        cg.add(var.set_filters(slot, await sensor.build_filters(filters)))

    # The sensors are created at runtime: reserve their entity slots and strings now.
    for _ in range(config[CONF_MAX_SENSORS]):
        CORE.register_platform_component("sensor", var)
    cg.add_define("USE_ENTITY_DEVICE_CLASS")
    cg.add_define("USE_ENTITY_UNIT_OF_MEASUREMENT")
    cg.add(
        var.set_entity_strings(
            register_device_class(DEVICE_CLASS_TEMPERATURE),
            register_unit_of_measurement(UNIT_CELSIUS),
        )
    )

    # Same group hash as web_server.add_entity_config() computes for YAML entities.
    if (sorting := config.get(CONF_WEB_SERVER)) is not None:
        server = await cg.get_variable(sorting[CONF_WEB_SERVER_ID])
        cg.add_define("USE_WEBSERVER_SORTING")
        cg.add(
            var.set_web_server_sorting(
                server,
                hash(sorting.get(web_server.CONF_SORTING_GROUP_ID)),
                sorting.get(web_server.CONF_SORTING_WEIGHT, 50),
            )
        )
