"""Test-only settings type over config_json, registered the way jxd_config registers its own."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import config_json
from esphome.const import CONF_ID

DEPENDENCIES = ["config_json", "switch"]
AUTO_LOAD = ["json"]

CONF_CONFIG_JSON_ID = "config_json_id"
CONF_APPLY_ID = "apply_id"

config_base_ns = cg.esphome_ns.namespace("config_base")
SettingsApplyComponent = config_base_ns.class_("SettingsApplyComponent", cg.Component)

test_settings_ns = cg.esphome_ns.namespace("test_settings")
TestSettingsJson = test_settings_ns.class_(
    "TestSettingsJson", config_json.SettingsBaseJson
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(TestSettingsJson),
        cv.GenerateID(CONF_CONFIG_JSON_ID): cv.use_id(config_json.ConfigJsonKeeper),
        # Declared, so ESPHOME_COMPONENT_COUNT covers it — exactly as jxd_config does.
        cv.GenerateID(CONF_APPLY_ID): cv.declare_id(
            SettingsApplyComponent.template(TestSettingsJson)
        ),
    }
)


async def to_code(config):
    keeper = await cg.get_variable(config[CONF_CONFIG_JSON_ID])
    settings = cg.new_Pvariable(config[CONF_ID])
    cg.add(keeper.add_settings(settings))
    apply = cg.new_Pvariable(config[CONF_APPLY_ID], settings)
    await cg.register_component(apply, {})
