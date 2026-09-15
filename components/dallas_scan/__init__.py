"""DS18B20 sensors created at boot, one per device on a 1-Wire bus; slot numbers stick in flash."""

import esphome.codegen as cg
from esphome.components import one_wire, sensor, web_server
import esphome.config_validation as cv
from esphome.const import (
    CONF_ADDRESS,
    CONF_FILTERS,
    CONF_ID,
    CONF_RESOLUTION,
    CONF_SENSORS,
    CONF_WEB_SERVER,
    CONF_WEB_SERVER_ID,
    DEVICE_CLASS_TEMPERATURE,
    UNIT_CELSIUS,
)
from esphome.core import CORE, ID
import esphome.final_validate as fv
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
CONF_ADDRESSES = "addresses"

dallas_scan_ns = cg.esphome_ns.namespace("dallas_scan")
DallasScan = dallas_scan_ns.class_("DallasScan", cg.PollingComponent)


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
    listed = len(config[CONF_SENSORS])
    if listed > config[CONF_MAX_SENSORS]:
        raise cv.Invalid(
            f"{listed} sensors listed, {CONF_MAX_SENSORS} is {config[CONF_MAX_SENSORS]}",
            path=[CONF_SENSORS],
        )
    for slot in config[CONF_ADDRESSES]:
        if slot > config[CONF_MAX_SENSORS]:
            raise cv.Invalid(
                f"Slot {slot} is above {CONF_MAX_SENSORS} ({config[CONF_MAX_SENSORS]})",
                path=[CONF_ADDRESSES, slot],
            )
        if slot <= listed:
            raise cv.Invalid(
                f"Slot {slot} is taken by {CONF_SENSORS}", path=[CONF_ADDRESSES, slot]
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
            # YAML sensors that take the first slots, in this order.
            cv.Optional(CONF_SENSORS, default=[]): cv.ensure_list(
                cv.use_id(sensor.Sensor)
            ),
            # Slot number -> ROM address; the slot is pinned to that device.
            cv.Optional(CONF_ADDRESSES, default={}): cv.Schema(
                {cv.int_range(min=1, max=64): cv.hex_uint64_t}
            ),
            # The usual sensor filters, the same chain on every sensor.
            cv.Optional(CONF_FILTERS): sensor.validate_filters,
        }
    )
    .extend(web_server.WEBSERVER_SORTING_SCHEMA)
    .extend(cv.polling_component_schema("60s")),
    _validate,
)


def _sensor_entry(full_config, sensor_id):
    """The sensor: entry a listed id refers to."""
    for entry in full_config.get("sensor") or []:
        if (entry_id := entry.get(CONF_ID)) is not None and entry_id.id == sensor_id.id:
            return entry
    return None


def _one_wire_address(entry):
    """The address: of a 1-Wire sensor entry, None for other sensors."""
    if entry is None or one_wire.CONF_ONE_WIRE_ID not in entry:
        return None
    return entry.get(CONF_ADDRESS)


def _final_validate(config):
    # A listed 1-Wire sensor pins its address to its slot; without one the scan
    # would hand the same device a slot of its own.
    full_config = fv.full_config.get()
    for index, sensor_id in enumerate(config[CONF_SENSORS]):
        entry = _sensor_entry(full_config, sensor_id)
        if entry is None or one_wire.CONF_ONE_WIRE_ID not in entry:
            continue
        if CONF_ADDRESS not in entry:
            raise cv.Invalid(
                f"{sensor_id.id} is a 1-Wire sensor: give it an {CONF_ADDRESS} "
                f"instead of an index to list it in {CONF_SENSORS}",
                path=[CONF_SENSORS, index],
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


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
    for slot, sensor_id in enumerate(config[CONF_SENSORS]):
        cg.add(var.set_sensor(slot, await cg.get_variable(sensor_id)))
        address = _one_wire_address(_sensor_entry(CORE.config, sensor_id))
        if address is not None:
            cg.add(var.pin(slot, address))
    for slot, address in config[CONF_ADDRESSES].items():
        cg.add(var.pin(slot - 1, address))
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
