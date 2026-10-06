"""Fits text to a row of the display menu: '?' for what the font cannot draw, cut by code points.

Nothing to configure: `panel_text:` makes the functions reachable from lambdas, and a component
whose C++ calls them auto-loads it instead.
"""

import esphome.config_validation as cv

CODEOWNERS = ["@jethome-iot"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    pass
