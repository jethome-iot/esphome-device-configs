"""Test-only key that pulls display_menu_base into a host build: it has none of its own."""

import esphome.config_validation as cv

AUTO_LOAD = ["display_menu_base"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
