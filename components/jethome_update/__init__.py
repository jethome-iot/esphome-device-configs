"""Firmware updates from a JetHome firmware server's device manifest."""

import esphome.codegen as cg

CODEOWNERS = ["@jethome-iot"]

jethome_update_ns = cg.esphome_ns.namespace("jethome_update")
