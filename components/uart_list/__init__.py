"""Numbered list of UART buses, so settings can refer to a bus by index."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["uart"]

CONF_UARTS = "uarts"

uart_list_ns = cg.esphome_ns.namespace("uart_list")
UartList = uart_list_ns.class_("UartList")

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UartList),
        cv.Required(CONF_UARTS): cv.ensure_list(cv.use_id(uart.UARTComponent)),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    for uart_id in config[CONF_UARTS]:
        cg.add(var.add_uart(await cg.get_variable(uart_id)))
