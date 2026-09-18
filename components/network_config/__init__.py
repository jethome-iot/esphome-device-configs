"""The stored network configuration: the WiFi network list and each interface's IP settings.

The record is kept by config_json; this component puts it into the interfaces at boot and owns
what the "Network mode" select and the device API do to it.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome.components import config_json, ethernet, select, wifi
from esphome.const import (
    CONF_ENABLE_ON_BOOT,
    CONF_ETHERNET,
    CONF_ID,
    CONF_MANUAL_IP,
    CONF_NETWORKS,
    CONF_WIFI,
    PLATFORM_ESP32,
    PLATFORM_HOST,
)

CODEOWNERS = ["@jethome-iot"]
DEPENDENCIES = ["config_json"]
AUTO_LOAD = ["json"]

CONF_CONFIG_JSON_ID = "config_json_id"
# The record, so lambdas and the device API can address it.
CONF_SETTINGS_ID = "settings_id"
CONF_NETWORK_MODE_SELECT_ID = "network_mode_select_id"
CONF_WIFI_ID = "wifi_id"
CONF_ETHERNET_ID = "ethernet_id"

network_config_ns = cg.esphome_ns.namespace("network_config")
NetworkConfig = network_config_ns.class_("NetworkConfig", cg.Component)
NetworkSettingsJson = network_config_ns.class_(
    "NetworkSettingsJson", config_json.SettingsBaseJson
)

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            # The only declared Component here, and it is always registered, so nothing has to
            # be popped from ESPHOME_COMPONENT_COUNT the way entity_config does for a disabled
            # type: App.components_ is a StaticVector sized by that macro.
            cv.GenerateID(): cv.declare_id(NetworkConfig),
            cv.GenerateID(CONF_CONFIG_JSON_ID): cv.use_id(config_json.ConfigJsonKeeper),
            # Not a Component subclass, so this id claims no slot.
            cv.GenerateID(CONF_SETTINGS_ID): cv.declare_id(NetworkSettingsJson),
            cv.Optional(CONF_NETWORK_MODE_SELECT_ID): cv.use_id(select.Select),
            cv.Optional(CONF_WIFI_ID): cv.use_id(wifi.WiFiComponent),
            cv.Optional(CONF_ETHERNET_ID): cv.use_id(ethernet.EthernetComponent),
        }
    ),
    cv.only_on([PLATFORM_ESP32, PLATFORM_HOST]),  # host: the test suite
)


def _final_validate(config):
    full = fv.full_config.get()

    if CONF_WIFI in full:
        if CONF_WIFI_ID not in config:
            raise cv.Invalid(
                "network_config needs 'wifi_id:' to reach the 'wifi:' section"
            )
        wifi_config = full[CONF_WIFI]
        # The station list is installed before the interfaces set up, and WiFiComponent::start()
        # replaces the whole list with its saved credentials. Only a component that starts
        # disabled gives the boot script a place to reinstall it after enable().
        if wifi_config.get(CONF_ENABLE_ON_BOOT, True):
            raise cv.Invalid(
                "network_config needs 'wifi:' with 'enable_on_boot: false'"
            )
        # Codegen emits init_sta()/add_sta() for a listed network too, and whichever runs last
        # wins.
        if wifi_config.get(CONF_NETWORKS):
            raise cv.Invalid(
                "network_config owns the WiFi network list; remove 'networks:' from 'wifi:'"
            )
        if CONF_MANUAL_IP in wifi_config:
            raise cv.Invalid(
                "network_config sets the WiFi address at run time; remove 'manual_ip:' from "
                "'wifi:' (it also retargets 'use_address')"
            )

    if CONF_ETHERNET in full:
        if CONF_ETHERNET_ID not in config:
            raise cv.Invalid(
                "network_config needs 'ethernet_id:' to reach the 'ethernet:' section"
            )
        if CONF_MANUAL_IP in full[CONF_ETHERNET]:
            raise cv.Invalid(
                "network_config sets the Ethernet address at run time; remove 'manual_ip:' "
                "from 'ethernet:' (it also retargets 'use_address')"
            )

    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    cg.add_define("USE_NETWORK_CONFIG")

    keeper = await cg.get_variable(config[CONF_CONFIG_JSON_ID])
    settings = cg.new_Pvariable(config[CONF_SETTINGS_ID])
    cg.add(keeper.add_settings(settings))

    var = cg.new_Pvariable(config[CONF_ID], settings)
    await cg.register_component(var, {})

    if select_id := config.get(CONF_NETWORK_MODE_SELECT_ID):
        cg.add(var.set_mode_select(await cg.get_variable(select_id)))

    if wifi_id := config.get(CONF_WIFI_ID):
        cg.add(var.set_wifi(await cg.get_variable(wifi_id)))
        # Upstream compiles WiFiAP::set_manual_ip() in only for a YAML that asked for a manual
        # IP (wifi/__init__.py:655-657), and a 'manual_ip:' placeholder there would retarget
        # 'use_address'.
        cg.add_define("USE_WIFI_MANUAL_IP")

    if ethernet_id := config.get(CONF_ETHERNET_ID):
        cg.add(var.set_ethernet(await cg.get_variable(ethernet_id)))
        # The same for EthernetComponent::set_manual_ip() (ethernet/__init__.py:585-587).
        cg.add_define("USE_ETHERNET_MANUAL_IP")
