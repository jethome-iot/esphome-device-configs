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
    sensors = config[CONF_SENSORS]
    listed = len(sensors)
    if listed > config[CONF_MAX_SENSORS]:
        raise cv.Invalid(
            f"{listed} sensors listed, {CONF_MAX_SENSORS} is {config[CONF_MAX_SENSORS]}",
            path=[CONF_SENSORS],
        )
    if len({sensor_id.id for sensor_id in sensors}) != listed:
        raise cv.Invalid("A sensor is listed twice", path=[CONF_SENSORS])
    # A filter chain belongs to one sensor, so every slot the component fills gets
    # its own copy; the id pass names the copies' ids after this.
    if filters := config.get(CONF_FILTERS):
        automatic = config[CONF_MAX_SENSORS] - listed
        config[CONF_FILTERS] = (
            [filters] + [_fresh_ids(filters) for _ in range(automatic - 1)]
            if automatic
            else []
        )
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
    listed = [await cg.get_variable(sensor_id) for sensor_id in config[CONF_SENSORS]]
    for slot, (sensor_id, listed_sensor) in enumerate(
        zip(config[CONF_SENSORS], listed)
    ):
        cg.add(var.set_sensor(slot, listed_sensor))
        # Its device keeps this slot, so the scan does not hand it another one.
        address = _one_wire_address(_sensor_entry(CORE.config, sensor_id))
        if address is not None:
            cg.add(var.pin(slot, address))
    if filters := config.get(CONF_FILTERS):
        cg.add_define("USE_SENSOR_FILTER")
        for slot, chain in enumerate(filters, start=len(listed)):
            cg.add(var.set_filters(slot, await sensor.build_filters(chain)))

    # The sensors are created at runtime: reserve their entity slots and strings now.
    for _ in range(config[CONF_MAX_SENSORS] - len(listed)):
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
    if sorting := config.get(CONF_WEB_SERVER):
        server = await cg.get_variable(sorting[CONF_WEB_SERVER_ID])
        group = hash(sorting.get(web_server.CONF_SORTING_GROUP_ID))
        weight = sorting.get(web_server.CONF_SORTING_WEIGHT, 50)
        cg.add_define("USE_WEBSERVER_SORTING")
        cg.add(var.set_web_server_sorting(server, group, weight))
        # Listed sensors join the group too, unless they sort themselves.
        for slot, (sensor_id, listed_sensor) in enumerate(
            zip(config[CONF_SENSORS], listed)
        ):
            entry = _sensor_entry(CORE.config, sensor_id)
            if entry is not None and entry.get(CONF_WEB_SERVER):
                continue
            cg.add(server.add_entity_config(listed_sensor, weight + slot, group))
