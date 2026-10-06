"""Test-only key that pulls ESPHome's pid component into a host build: its autotuner is the
reference climate_hub's port is checked against. pid has no key of its own, and its climate
needs output."""

import esphome.config_validation as cv

AUTO_LOAD = ["output", "pid"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
