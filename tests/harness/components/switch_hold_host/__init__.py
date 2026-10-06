"""Test-only key that pulls switch_hold into a host build: it has none of its own."""

import esphome.config_validation as cv

AUTO_LOAD = ["switch_hold"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
