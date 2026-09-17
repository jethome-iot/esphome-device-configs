"""Host stand-in for upstream's web_server, which builds for ESP platforms only.

web_device_dashboard depends on it for the entity REST API its page calls, not for anything
this suite drives, so the stand-in only carries what the dependency and the entity platforms
ask of it: the per-entity sorting block, which nothing here declares.
"""

import esphome.config_validation as cv

# As upstream's: the dashboard builds its answers with json::build_json.
AUTO_LOAD = ["json"]

CONFIG_SCHEMA = cv.Schema({})

WEBSERVER_SORTING_SCHEMA = cv.Schema({})


async def to_code(config):
    pass


async def add_entity_config(entity, config):
    pass
