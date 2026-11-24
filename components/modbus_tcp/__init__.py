"""Serves a modbus_server's map over Modbus TCP, through the server's own request handlers."""

import esphome.codegen as cg
from esphome.components import socket
from esphome.components.const import CONF_ENABLED
from esphome.components.modbus_server import ModbusServer
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_PORT

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["modbus_server", "network"]
AUTO_LOAD = ["socket"]

CONF_MODBUS_SERVER_ID = "modbus_server_id"
CONF_UNIT_ID = "unit_id"
CONF_MAX_CLIENTS = "max_clients"
CONF_IDLE_TIMEOUT = "idle_timeout"
CONF_SERVE_WIFI_AP = "serve_wifi_ap"

modbus_tcp_ns = cg.esphome_ns.namespace("modbus_tcp")
ModbusTcpServer = modbus_tcp_ns.class_("ModbusTcpServer", cg.Component)


def _unit_id(value):
    value = cv.int_(value)
    if value in (0, 255):
        raise cv.Invalid(
            f"unit {value} always reaches the device; give its own unit id, 1-247"
        )
    return cv.int_range(min=1, max=247)(value)


def _consume_sockets(config):
    # Each client holds a socket for as long as it stays connected, beside the listener.
    socket.consume_sockets(config[CONF_MAX_CLIENTS], "modbus_tcp")(config)
    socket.consume_sockets(1, "modbus_tcp", socket.SocketType.TCP_LISTEN)(config)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ModbusTcpServer),
            cv.Required(CONF_MODBUS_SERVER_ID): cv.use_id(ModbusServer),
            cv.Optional(CONF_ENABLED, default=False): cv.boolean,
            cv.Optional(CONF_PORT, default=502): cv.port,
            # 0 and 255 always reach the device: they are what a client sends to a plain TCP device.
            cv.Optional(CONF_UNIT_ID, default=1): _unit_id,
            cv.Optional(CONF_MAX_CLIENTS, default=2): cv.int_range(min=1, max=8),
            cv.Optional(
                CONF_IDLE_TIMEOUT, default="60s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_SERVE_WIFI_AP, default=False): cv.boolean,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _consume_sockets,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_server(await cg.get_variable(config[CONF_MODBUS_SERVER_ID])))
    cg.add(var.set_enabled(config[CONF_ENABLED]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_unit_id(config[CONF_UNIT_ID]))
    cg.add(var.set_max_clients(config[CONF_MAX_CLIENTS]))
    cg.add(var.set_idle_timeout(config[CONF_IDLE_TIMEOUT]))
    cg.add(var.set_serve_wifi_ap(config[CONF_SERVE_WIFI_AP]))
