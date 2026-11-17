"""Folds the case of names, the same way on the device and in the dashboard's clients.

Nothing to configure: a component that compares names auto-loads it.
"""

import esphome.config_validation as cv

CODEOWNERS = ["@jethome-iot"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
