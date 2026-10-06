"""A modbus_server's address map as named ranges, checked against the server's own config at build time."""

import esphome.codegen as cg
from esphome.components.const import CONF_ENABLED
from esphome.components.modbus.helpers import TYPE_REGISTER_MAP
from esphome.components.modbus_server import ModbusServer
from esphome.components.modbus_server.const import (
    CONF_BITS,
    CONF_COURTESY_RESPONSE,
    CONF_REGISTER_LAST_ADDRESS,
    CONF_REGISTER_VALUE,
    CONF_REGISTERS,
    CONF_VALUE_TYPE,
    CONF_WRITE_LAMBDA,
)
import esphome.config_validation as cv
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_NAME
from esphome.core import CORE
import esphome.final_validate as fv

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["modbus_server"]

CONF_MODBUS_SERVER_ID = "modbus_server_id"
CONF_SCALE = "scale"
CONF_UNIT = "unit"
CONF_NO_VALUE = "no_value"

# What a table's values are called in a message.
KIND = {CONF_BITS: "bit", CONF_REGISTERS: "register"}

modbus_map_ns = cg.esphome_ns.namespace("modbus_map")
ModbusMap = modbus_map_ns.class_("ModbusMap")
BitRange = modbus_map_ns.struct("BitRange")
RegisterRange = modbus_map_ns.struct("RegisterRange")


def _text(value):
    value = cv.string_strict(value)
    if not value.strip():
        raise cv.Invalid("must not be empty")
    return value


BIT_RANGE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_ADDRESS): cv.hex_uint16_t,
        cv.Required(CONF_NAME): _text,
    }
)

REGISTER_RANGE_SCHEMA = BIT_RANGE_SCHEMA.extend(
    {
        cv.Optional(CONF_SCALE): cv.positive_not_null_float,
        cv.Optional(CONF_UNIT): _text,
        cv.Optional(CONF_NO_VALUE): cv.hex_uint16_t,
    }
)


def _unique_addresses(config):
    for table in (CONF_BITS, CONF_REGISTERS):
        seen = set()
        for index, entry in enumerate(config.get(table, [])):
            address = entry[CONF_ADDRESS]
            if address in seen:
                raise cv.Invalid(
                    f"0x{address:04X} is named twice under {table}",
                    path=[table, index, CONF_ADDRESS],
                )
            seen.add(address)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ModbusMap),
            cv.Required(CONF_MODBUS_SERVER_ID): cv.use_id(ModbusServer),
            cv.Optional(CONF_BITS): cv.All(
                cv.ensure_list(BIT_RANGE_SCHEMA), cv.Length(min=1)
            ),
            cv.Optional(CONF_REGISTERS): cv.All(
                cv.ensure_list(REGISTER_RANGE_SCHEMA), cv.Length(min=1)
            ),
        }
    ),
    cv.has_at_least_one_key(CONF_BITS, CONF_REGISTERS),
    _unique_addresses,
)


def _span(first, last):
    return f"0x{first:04X}" if first == last else f"0x{first:04X}-0x{last:04X}"


def _server(config, full_config):
    server_id = config[CONF_MODBUS_SERVER_ID].id
    for server in full_config.get("modbus_server") or []:
        if server[CONF_ID].id == server_id:
            return server
    raise cv.Invalid(
        f"There is no modbus_server with id {server_id}",
        path=[CONF_MODBUS_SERVER_ID],
    )


def _served(server, table):
    """(address, words, writable, value_type) of each value the server serves in `table`, by address."""
    values = []
    for entry in server.get(table) or []:
        value_type = str(entry[CONF_VALUE_TYPE]) if table == CONF_REGISTERS else None
        words = TYPE_REGISTER_MAP[value_type] if value_type else 1
        values.append(
            (int(entry[CONF_ADDRESS]), words, CONF_WRITE_LAMBDA in entry, value_type)
        )
    return sorted(values)


def _check_named(server_id, table, served, named):
    starts = {value[0]: value for value in served}
    for index, entry in enumerate(named):
        address = entry[CONF_ADDRESS]
        if address in starts:
            words, value_type = starts[address][1], starts[address][3]
            if CONF_NO_VALUE in entry and words > 1:
                raise cv.Invalid(
                    f"no_value is one register, but the {value_type} at 0x{address:04X} "
                    f"spans {words}: remove no_value",
                    path=[table, index, CONF_NO_VALUE],
                )
            continue
        holder = next((v for v in served if v[0] < address < v[0] + v[1]), None)
        if holder is not None:
            raise cv.Invalid(
                f"0x{address:04X} is inside the {holder[3]} {server_id} serves at "
                f"0x{holder[0]:04X}: name 0x{holder[0]:04X} instead",
                path=[table, index, CONF_ADDRESS],
            )
        raise cv.Invalid(
            f"{server_id} serves no {KIND[table]} at 0x{address:04X}: "
            f"remove the entry or fix its address",
            path=[table, index, CONF_ADDRESS],
        )


def _runs(served, names):
    """Consecutive values alike in access and type, split where the map names an address."""
    runs = []
    for address, words, writable, value_type in served:
        run = runs[-1] if runs else None
        if (
            run is None
            or address != run["next"]
            or writable != run["writable"]
            or value_type != run["value_type"]
            or address in names
        ):
            run = {
                "address": address,
                "count": 0,
                "writable": writable,
                "value_type": value_type,
            }
            runs.append(run)
        run["count"] += 1
        run["next"] = address + words
        run["last_address"] = address + words - 1
    return runs


def derive_map(config, full_config):
    """The ranges the server serves, named by the map: {bits, registers, courtesy_response}.

    Raises cv.Invalid when the map and the server disagree.
    """
    server = _server(config, full_config)
    server_id = server[CONF_ID].id
    derived = {}
    for table in (CONF_BITS, CONF_REGISTERS):
        served = _served(server, table)
        named = config.get(table, [])
        _check_named(server_id, table, served, named)
        names = {entry[CONF_ADDRESS]: entry for entry in named}
        ranges = []
        for run in _runs(served, names):
            entry = names.get(run["address"])
            if entry is None:
                first, last = run["address"], run["last_address"]
                raise cv.Invalid(
                    f"{server_id} serves {KIND[table]}{'' if first == last else 's'} "
                    f"{_span(first, last)} that modbus_map does not name: add an entry "
                    f"under {table} with address: 0x{first:04X}",
                    path=[table] if table in config else [],
                )
            out = {
                "address": run["address"],
                "last_address": run["last_address"],
                "count": run["count"],
                "writable": run["writable"],
                "name": entry[CONF_NAME],
            }
            if table == CONF_REGISTERS:
                out["value_type"] = run["value_type"]
                for key in (CONF_SCALE, CONF_UNIT, CONF_NO_VALUE):
                    if key in entry:
                        out[key] = entry[key]
            ranges.append(out)
        derived[table] = ranges
    courtesy = server.get(CONF_COURTESY_RESPONSE)
    derived["courtesy_response"] = (
        {
            "last_address": courtesy[CONF_REGISTER_LAST_ADDRESS],
            "value": courtesy[CONF_REGISTER_VALUE],
        }
        if courtesy and courtesy[CONF_ENABLED]
        else None
    )
    return derived


def _final_validate(config):
    derive_map(config, fv.full_config.get())
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def _fields(entry, keys):
    return [(key, entry.get(key)) for key in keys]


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    # Final validation keeps nothing, so the ranges are derived again from the same config.
    derived = derive_map(config, CORE.config)
    common = ("address", "last_address", "count", "writable", "name")
    for entry in derived[CONF_BITS]:
        cg.add(var.add_bits(cg.StructInitializer(BitRange, *_fields(entry, common))))
    for entry in derived[CONF_REGISTERS]:
        keys = (*common, "value_type", CONF_SCALE, CONF_UNIT, CONF_NO_VALUE)
        cg.add(
            var.add_registers(
                cg.StructInitializer(RegisterRange, *_fields(entry, keys))
            )
        )
    if (courtesy := derived["courtesy_response"]) is not None:
        cg.add(var.set_courtesy_response(courtesy["last_address"], courtesy["value"]))
