"""Host stand-in for upstream's mqtt, which builds for ESP platforms and LibreTiny only.

Its schema and codegen are upstream's own, loaded from the installed ESPHome under a private
name because this module shadows it: the keys, the defaults and every setter call stay exact,
so mqtt_config's final_validate sees the real block and its tests see what codegen set. Only
the platform check and the socket accounting are dropped. The C++ next to this file replaces
the client and the entity classes with ones a test drives in place of a broker.
"""

import importlib.util
from pathlib import Path

import esphome
import esphome.config_validation as cv

_spec = importlib.util.spec_from_file_location(
    "esphome_upstream_mqtt",
    Path(esphome.__file__).parent / "components" / "mqtt" / "__init__.py",
)
_up = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_up)

DEPENDENCIES = ["network"]
AUTO_LOAD = ["json"]

# The classes the entity domains read at import time (mqtt.MQTTSensorComponent, …).
for _name in dir(_up):
    if _name.startswith("MQTT") or _name == "mqtt_ns":
        globals()[_name] = getattr(_up, _name)

CONFIG_SCHEMA = cv.All(_up.CONFIG_SCHEMA.validators[0], _up.validate_config)

to_code = _up.to_code
register_mqtt_component = _up.register_mqtt_component
