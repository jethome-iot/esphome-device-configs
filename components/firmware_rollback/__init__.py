"""Boots the firmware in the other app slot: whether there is one to go back to, and the switch.

A library: web_device_dashboard auto-loads it for its rollback route, and a display menu
declares it to offer the same rollback from its own rows.
"""

import esphome.config_validation as cv
from esphome.const import PLATFORM_ESP32, PLATFORM_HOST

CODEOWNERS = ["@jethome-iot"]

# Nothing to configure.
CONFIG_SCHEMA = cv.All(
    cv.Schema({}),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)
