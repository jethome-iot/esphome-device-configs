"""The MQTT client's settings, kept on the device and set from the dashboard.

The stock `mqtt:` client is compiled idle with an empty broker; this component applies the
stored settings at boot and turns the client on when they say so. final_validate holds the
`mqtt:` block to the shape that makes that safe.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import logger, mqtt
from esphome.const import (
    CONF_BIRTH_MESSAGE,
    CONF_BROKER,
    CONF_CERTIFICATE_AUTHORITY,
    CONF_CLEAN_SESSION,
    CONF_CLIENT_CERTIFICATE,
    CONF_CLIENT_CERTIFICATE_KEY,
    CONF_CLIENT_ID,
    CONF_DISCOVER_IP,
    CONF_DISCOVERY,
    CONF_DISCOVERY_RETAIN,
    CONF_DISCOVERY_UNIQUE_ID_GENERATOR,
    CONF_ENABLE_ON_BOOT,
    CONF_ID,
    CONF_LOG_TOPIC,
    CONF_MQTT_ID,
    CONF_PAYLOAD,
    CONF_QOS,
    CONF_REBOOT_TIMEOUT,
    CONF_RETAIN,
    CONF_SHUTDOWN_MESSAGE,
    CONF_TOPIC,
    CONF_TOPIC_PREFIX,
    CONF_WILL_MESSAGE,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)
from esphome.core import CORE, ID
from esphome.helpers import fnv1_hash

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["mqtt"]
AUTO_LOAD = ["json"]

CONF_MQTT_PARENT_ID = "mqtt_parent_id"
CONF_WAIT_FOR_CONNECTION = "wait_for_connection"
CONF_MQTT = "mqtt"
CONF_DALLAS_SCAN = "dallas_scan"

mqtt_config_ns = cg.esphome_ns.namespace("mqtt_config")
MqttConfig = mqtt_config_ns.class_("MqttConfig", cg.Component)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(MqttConfig),
            cv.GenerateID(CONF_MQTT_PARENT_ID): cv.use_id(mqtt.MQTTClientComponent),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)

STATUS_MESSAGES = (CONF_BIRTH_MESSAGE, CONF_WILL_MESSAGE, CONF_SHUTDOWN_MESSAGE)
TLS_KEYS = (
    CONF_CERTIFICATE_AUTHORITY,
    CONF_CLIENT_CERTIFICATE,
    CONF_CLIENT_CERTIFICATE_KEY,
)


def _check_mqtt_block(conf):
    """Why the merged mqtt: block cannot carry device-set settings, or None. In table order."""
    if conf.get(CONF_BROKER) != "":
        return (
            "mqtt_config sets the broker on the device: leave 'broker: \"\"' in the "
            "mqtt: block"
        )
    if conf.get(CONF_ENABLE_ON_BOOT):
        return "mqtt_config starts the client once it has a broker: set 'enable_on_boot: false'"
    if conf[CONF_REBOOT_TIMEOUT].total_milliseconds != 0:
        return (
            "mqtt_config needs 'reboot_timeout: 0s': a mistyped broker would reboot the "
            "device every 15 minutes"
        )
    if not conf.get(CONF_CLEAN_SESSION):
        return (
            "set 'clean_session: true': a kept session leaves removed topics subscribed "
            "on the broker"
        )
    if conf.get(CONF_DISCOVERY) is not True:
        return (
            "mqtt_config switches Home Assistant discovery on the device: set "
            "'discovery: true'"
        )
    if not conf.get(CONF_DISCOVERY_RETAIN):
        return (
            "set 'discovery_retain: true': Home Assistant loses unretained entries when "
            "it restarts"
        )
    if str(conf.get(CONF_DISCOVERY_UNIQUE_ID_GENERATOR)) != "mac":
        return "set 'discovery_unique_id_generator: mac': legacy ids collide between devices"
    if conf.get(CONF_DISCOVER_IP):
        return "set 'discover_ip: false': the native API already announces the device"
    if conf.get(CONF_LOG_TOPIC):
        return (
            "set 'log_topic: null': the logger runs at debug level and every line would "
            "go to the broker"
        )
    if conf.get(CONF_WAIT_FOR_CONNECTION):
        return (
            "'wait_for_connection' would hold the boot until a broker nobody may have "
            "set answers"
        )
    if conf.get(CONF_TOPIC_PREFIX) != CORE.name:
        return "the topic prefix is set on the device: leave 'topic_prefix' out"
    # The default the dashboard shows is the client's own, which a compiled one would replace.
    if CONF_CLIENT_ID in conf:
        return "the client ID is set on the device: leave 'client_id' out"
    for key in STATUS_MESSAGES:
        # None or {} means no such message, which the device keeps.
        if (message := conf.get(key)) and message[CONF_TOPIC] != f"{CORE.name}/status":
            return (
                f"mqtt_config points '{key}' at '<topic prefix>/status' per device: leave "
                "its 'topic' out"
            )
    for key in TLS_KEYS:
        if key in conf:
            return f"mqtt_config has no TLS yet: remove '{key}'"
    return None


def _final_validate(config):
    if (fault := _check_mqtt_block(fv.full_config.get()[CONF_MQTT])) is not None:
        raise cv.Invalid(fault, path=[CONF_MQTT])
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


def mqtt_entities(node):
    """(mqtt_id, entity id) of every entity that has an MQTT component, in config order."""
    if isinstance(node, dict):
        if isinstance(mqtt_id := node.get(CONF_MQTT_ID), ID) and isinstance(
            entity_id := node.get(CONF_ID), ID
        ):
            yield mqtt_id, entity_id
        for value in node.values():
            yield from mqtt_entities(value)
    elif isinstance(node, list):
        for value in node:
            yield from mqtt_entities(value)


async def to_code(config):
    cg.add_define("USE_MQTT_CONFIG")

    client = await cg.get_variable(config[CONF_MQTT_PARENT_ID])
    var = cg.new_Pvariable(config[CONF_ID], client)
    await cg.register_component(var, config)
    cg.add(var.set_preference_hash(fnv1_hash(config[CONF_ID].id)))

    mqtt_conf = CORE.config[CONF_MQTT]
    for key, setter in (
        (CONF_BIRTH_MESSAGE, var.set_birth_template),
        (CONF_WILL_MESSAGE, var.set_will_template),
        (CONF_SHUTDOWN_MESSAGE, var.set_shutdown_template),
    ):
        if message := mqtt_conf.get(key):
            cg.add(
                setter(message[CONF_PAYLOAD], message[CONF_QOS], message[CONF_RETAIN])
            )

    # Its own slot: the client's connection errors arrive only as log lines.
    if "logger" in CORE.config:
        logger.request_log_listener()

    # Hidden ones too: the C++ skips what the client keeps internal.
    entities = list(mqtt_entities(CORE.config))
    cg.add(var.reserve_entities(len(entities)))
    for mqtt_id, entity_id in entities:
        cg.add(
            var.add_entity(
                await cg.get_variable(mqtt_id), await cg.get_variable(entity_id)
            )
        )

    # Temp N are created at setup, with no MQTT component of their own.
    if (scan := CORE.config.get(CONF_DALLAS_SCAN)) is not None:
        temps = await cg.get_variable(scan[CONF_ID])
        cg.add(
            var.add_runtime_sensors(
                cg.RawExpression(f"[]() {{ return {temps}->sensors(); }}")
            )
        )
