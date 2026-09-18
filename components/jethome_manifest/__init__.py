"""A JetHome firmware server's device manifest: what its JSON says about the latest firmware."""

import esphome.codegen as cg
import esphome.config_validation as cv

CODEOWNERS = ["@jethome-iot"]
# update: for the UpdateInfo the parser fills.
AUTO_LOAD = ["json", "update"]

jethome_manifest_ns = cg.esphome_ns.namespace("jethome_manifest")

# Nothing to configure: the parser is a library the update platform auto-loads.
CONFIG_SCHEMA = cv.Schema({})
