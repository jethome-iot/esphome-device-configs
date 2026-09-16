"""Per-switch and per-binary_sensor settings kept by config_json, applied at boot and editable at run time.

Each type is opt-in via `settings:`. Here the display menu is the only editor; the REST endpoint
the record hooks were written for is not part of this repository.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import config_json

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["config_json"]
AUTO_LOAD = ["json"]

CONF_CONFIG_JSON_ID = "config_json_id"
CONF_SETTINGS = "settings"
# The settings objects, so lambdas (the display menu) can address them.
CONF_SWITCH_SETTINGS_ID = "switch_settings_id"
CONF_BINARY_SENSOR_SETTINGS_ID = "binary_sensor_settings_id"
# The apply components. Declared per type so ESPHOME_COMPONENT_COUNT covers them:
# App.components_ is a StaticVector sized by that macro and drops silently when full.
CONF_SWITCH_APPLY_ID = "switch_apply_id"
CONF_BINARY_SENSOR_APPLY_ID = "binary_sensor_apply_id"

SETTING_SWITCH = "switch"
SETTING_BINARY_SENSOR = "binary_sensor"
SETTINGS_TYPES = [SETTING_SWITCH, SETTING_BINARY_SENSOR]

config_base_ns = cg.esphome_ns.namespace("config_base")
SettingsApplyComponent = config_base_ns.class_("SettingsApplyComponent", cg.Component)

entity_config_ns = cg.esphome_ns.namespace("entity_config")
SwitchSettingsJson = entity_config_ns.class_(
    "SwitchSettingsJson", config_json.SettingsBaseJson
)
BinarySensorSettingsJson = entity_config_ns.class_(
    "BinarySensorSettingsJson", config_json.SettingsBaseJson
)

SETTINGS_CLASSES = {
    SETTING_SWITCH: (CONF_SWITCH_SETTINGS_ID, CONF_SWITCH_APPLY_ID, SwitchSettingsJson),
    SETTING_BINARY_SENSOR: (
        CONF_BINARY_SENSOR_SETTINGS_ID,
        CONF_BINARY_SENSOR_APPLY_ID,
        BinarySensorSettingsJson,
    ),
}


def _validate(config):
    enabled = config[CONF_SETTINGS]
    # An id declared here reserves a slot in ESPHOME_COMPONENT_COUNT whether or not
    # anything registers it, so a disabled type must not leave one behind.
    for setting, (_, apply_key, _cls) in SETTINGS_CLASSES.items():
        if setting not in enabled:
            config.pop(apply_key, None)
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(CONF_CONFIG_JSON_ID): cv.use_id(config_json.ConfigJsonKeeper),
            cv.Optional(CONF_SETTINGS, default=SETTINGS_TYPES): cv.ensure_list(
                cv.one_of(*SETTINGS_TYPES, lower=True)
            ),
            cv.GenerateID(CONF_SWITCH_SETTINGS_ID): cv.declare_id(SwitchSettingsJson),
            cv.GenerateID(CONF_BINARY_SENSOR_SETTINGS_ID): cv.declare_id(
                BinarySensorSettingsJson
            ),
        }
    ).extend(
        {
            cv.GenerateID(apply_key): cv.declare_id(
                SettingsApplyComponent.template(cls)
            )
            for _, apply_key, cls in SETTINGS_CLASSES.values()
        }
    ),
    cv.only_on_esp32,
    _validate,
)


def _final_validate(config):
    full = fv.full_config.get()
    for setting in config[CONF_SETTINGS]:
        if setting not in full:
            raise cv.Invalid(
                f"'{setting}' in '{CONF_SETTINGS}' needs a '{setting}:' section"
            )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def _add_settings(keeper, config, name):
    settings_key, apply_key, _cls = SETTINGS_CLASSES[name]
    settings = cg.new_Pvariable(config[settings_key])
    cg.add(keeper.add_settings(settings))
    # A regular component, so App::setup() sorts it by APPLY_PRIORITY with the rest.
    apply = cg.new_Pvariable(config[apply_key], settings)
    await cg.register_component(apply, {})
    return settings


async def to_code(config):
    json_keeper = await cg.get_variable(config[CONF_CONFIG_JSON_ID])
    enabled = config[CONF_SETTINGS]

    if SETTING_SWITCH in enabled:
        cg.add_define("ENTITY_CONFIG_SWITCH")
        await _add_settings(json_keeper, config, SETTING_SWITCH)

    if SETTING_BINARY_SENSOR in enabled:
        cg.add_define("ENTITY_CONFIG_BINARY_SENSOR")
        # Inversion is a filter appended at run time; the chain only compiles with this.
        cg.add_define("USE_BINARY_SENSOR_FILTER")
        await _add_settings(json_keeper, config, SETTING_BINARY_SENSOR)
