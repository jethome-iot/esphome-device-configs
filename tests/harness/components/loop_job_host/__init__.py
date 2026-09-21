"""Test-only key that pulls loop_job into a host build: it has none of its own."""

import esphome.config_validation as cv

AUTO_LOAD = ["loop_job"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
