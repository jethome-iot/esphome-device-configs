"""Settings kept by config_json (files) and config_nvs, applied at boot and editable at run time.

switch, binary_sensor, uart, timezone and mqtt live in JSON; auth in NVS. Each type is opt-in via
`settings:`; the dashboard edits switch/binary_sensor records over /api/device/entity-settings.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import config_json, config_nvs, uart_list
from esphome.components import time as time_component

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["config_json"]
AUTO_LOAD = ["json"]

CONF_CONFIG_JSON_ID = "config_json_id"
CONF_CONFIG_NVS_ID = "config_nvs_id"
CONF_UART_LIST_ID = "uart_list_id"
CONF_TIME_IDS = "time_ids"
CONF_SETTINGS = "settings"
# The settings objects, so lambdas (the display menu) can address them.
CONF_SWITCH_SETTINGS_ID = "switch_settings_id"
CONF_BINARY_SENSOR_SETTINGS_ID = "binary_sensor_settings_id"
CONF_UART_SETTINGS_ID = "uart_settings_id"
CONF_TIMEZONE_SETTINGS_ID = "timezone_settings_id"
CONF_MQTT_SETTINGS_ID = "mqtt_settings_id"
CONF_AUTH_SETTINGS_ID = "auth_settings_id"
# The apply components. Declared per type so ESPHOME_COMPONENT_COUNT covers them:
# App.components_ is a StaticVector sized by that macro and drops silently when full.
CONF_SWITCH_APPLY_ID = "switch_apply_id"
CONF_BINARY_SENSOR_APPLY_ID = "binary_sensor_apply_id"
CONF_UART_APPLY_ID = "uart_apply_id"
CONF_TIMEZONE_APPLY_ID = "timezone_apply_id"
CONF_MQTT_APPLY_ID = "mqtt_apply_id"
CONF_AUTH_APPLY_ID = "auth_apply_id"

SETTING_SWITCH = "switch"
SETTING_BINARY_SENSOR = "binary_sensor"
SETTING_UART = "uart"
SETTING_TIMEZONE = "timezone"
SETTING_MQTT = "mqtt"
SETTING_AUTH = "auth"
SETTINGS_TYPES = [
    SETTING_SWITCH,
    SETTING_BINARY_SENSOR,
    SETTING_UART,
    SETTING_TIMEZONE,
    SETTING_MQTT,
    SETTING_AUTH,
]
DEFAULT_SETTINGS = [SETTING_SWITCH, SETTING_BINARY_SENSOR]

config_base_ns = cg.esphome_ns.namespace("config_base")
SettingsApplyComponent = config_base_ns.class_("SettingsApplyComponent", cg.Component)

jxd_config_ns = cg.esphome_ns.namespace("jxd_config")
SwitchSettingsJson = jxd_config_ns.class_(
    "SwitchSettingsJson", config_json.SettingsBaseJson
)
BinarySensorSettingsJson = jxd_config_ns.class_(
    "BinarySensorSettingsJson", config_json.SettingsBaseJson
)
UartSettingsJson = jxd_config_ns.class_(
    "UartSettingsJson", config_json.SettingsBaseJson
)
TimezoneSettingsJson = jxd_config_ns.class_(
    "TimezoneSettingsJson", config_json.SettingsBaseJson
)
MqttSettingsJson = jxd_config_ns.class_(
    "MqttSettingsJson", config_json.SettingsBaseJson
)
AuthSettingsNvs = jxd_config_ns.class_("AuthSettingsNvs", config_nvs.SettingsBaseNvs)

SETTINGS_CLASSES = {
    SETTING_SWITCH: (CONF_SWITCH_SETTINGS_ID, CONF_SWITCH_APPLY_ID, SwitchSettingsJson),
    SETTING_BINARY_SENSOR: (
        CONF_BINARY_SENSOR_SETTINGS_ID,
        CONF_BINARY_SENSOR_APPLY_ID,
        BinarySensorSettingsJson,
    ),
    SETTING_UART: (CONF_UART_SETTINGS_ID, CONF_UART_APPLY_ID, UartSettingsJson),
    SETTING_TIMEZONE: (
        CONF_TIMEZONE_SETTINGS_ID,
        CONF_TIMEZONE_APPLY_ID,
        TimezoneSettingsJson,
    ),
    SETTING_MQTT: (CONF_MQTT_SETTINGS_ID, CONF_MQTT_APPLY_ID, MqttSettingsJson),
    SETTING_AUTH: (CONF_AUTH_SETTINGS_ID, CONF_AUTH_APPLY_ID, AuthSettingsNvs),
}


def _validate(config):
    enabled = config[CONF_SETTINGS]
    if SETTING_UART in enabled and CONF_UART_LIST_ID not in config:
        raise cv.Invalid(
            f"'{SETTING_UART}' in '{CONF_SETTINGS}' needs '{CONF_UART_LIST_ID}'"
        )
    if SETTING_TIMEZONE in enabled and not config.get(CONF_TIME_IDS):
        raise cv.Invalid(
            f"'{SETTING_TIMEZONE}' in '{CONF_SETTINGS}' needs '{CONF_TIME_IDS}'"
        )
    if SETTING_AUTH in enabled and CONF_CONFIG_NVS_ID not in config:
        raise cv.Invalid(
            f"'{SETTING_AUTH}' in '{CONF_SETTINGS}' needs '{CONF_CONFIG_NVS_ID}'"
        )
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
            cv.Optional(CONF_CONFIG_NVS_ID): cv.use_id(config_nvs.ConfigNvsKeeper),
            cv.Optional(CONF_UART_LIST_ID): cv.use_id(uart_list.UartList),
            cv.Optional(CONF_TIME_IDS): cv.ensure_list(
                cv.use_id(time_component.RealTimeClock)
            ),
            cv.Optional(CONF_SETTINGS, default=DEFAULT_SETTINGS): cv.ensure_list(
                cv.one_of(*SETTINGS_TYPES, lower=True)
            ),
            cv.GenerateID(CONF_SWITCH_SETTINGS_ID): cv.declare_id(SwitchSettingsJson),
            cv.GenerateID(CONF_BINARY_SENSOR_SETTINGS_ID): cv.declare_id(
                BinarySensorSettingsJson
            ),
            cv.GenerateID(CONF_UART_SETTINGS_ID): cv.declare_id(UartSettingsJson),
            cv.GenerateID(CONF_TIMEZONE_SETTINGS_ID): cv.declare_id(
                TimezoneSettingsJson
            ),
            cv.GenerateID(CONF_MQTT_SETTINGS_ID): cv.declare_id(MqttSettingsJson),
            cv.GenerateID(CONF_AUTH_SETTINGS_ID): cv.declare_id(AuthSettingsNvs),
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
    enabled = config[CONF_SETTINGS]
    for setting in (SETTING_SWITCH, SETTING_BINARY_SENSOR, SETTING_MQTT):
        if setting in enabled and setting not in full:
            raise cv.Invalid(
                f"'{setting}' in '{CONF_SETTINGS}' needs a '{setting}:' section"
            )
    if SETTING_AUTH in enabled and "auth" not in (full.get("web_server") or {}):
        raise cv.Invalid(
            f"'{SETTING_AUTH}' in '{CONF_SETTINGS}' needs 'web_server: auth:'"
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
        cg.add_define("JXD_CONFIG_SWITCH")
        await _add_settings(json_keeper, config, SETTING_SWITCH)

    if SETTING_BINARY_SENSOR in enabled:
        cg.add_define("JXD_CONFIG_BINARY_SENSOR")
        # Inversion is a filter appended at run time; the chain only compiles with this.
        cg.add_define("USE_BINARY_SENSOR_FILTER")
        await _add_settings(json_keeper, config, SETTING_BINARY_SENSOR)

    if SETTING_UART in enabled:
        cg.add_define("JXD_CONFIG_UART")
        settings = await _add_settings(json_keeper, config, SETTING_UART)
        cg.add(settings.set_uart_list(await cg.get_variable(config[CONF_UART_LIST_ID])))

    if SETTING_TIMEZONE in enabled:
        cg.add_define("JXD_CONFIG_TIMEZONE")
        settings = await _add_settings(json_keeper, config, SETTING_TIMEZONE)
        for time_id in config[CONF_TIME_IDS]:
            cg.add(settings.add_time_component(await cg.get_variable(time_id)))

    if SETTING_MQTT in enabled:
        cg.add_define("JXD_CONFIG_MQTT")
        await _add_settings(json_keeper, config, SETTING_MQTT)

    if SETTING_AUTH in enabled:
        cg.add_define("JXD_CONFIG_AUTH")
        nvs_keeper = await cg.get_variable(config[CONF_CONFIG_NVS_ID])
        await _add_settings(nvs_keeper, config, SETTING_AUTH)
