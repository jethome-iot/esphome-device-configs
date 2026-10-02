#pragma once

#include <string>
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/helpers.h"
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

// A platform with no configured entity has no registry in App; the names must still exist.
namespace esphome::sensor {
class Sensor;
}
namespace esphome::switch_ {
class Switch;
}

namespace esphome::climate_hub {

inline std::string object_id_of(const EntityBase &entity) {
  char buf[OBJECT_ID_MAX_LEN];
  return std::string(entity.get_object_id_to(buf));
}

/// The visible entity whose object id is `object_id`: the hash narrows, the string decides.
template<typename T, typename V> T *find_entity(const V &entities, const std::string &object_id) {
  const uint32_t key = fnv1_hash(object_id);
  for (auto *entity : entities) {
    if (entity->get_object_id_hash() == key && !entity->is_internal() && object_id_of(*entity) == object_id)
      return entity;
  }
  return nullptr;
}

inline sensor::Sensor *find_sensor(const std::string &object_id) {
#ifdef USE_SENSOR
  return find_entity<sensor::Sensor>(App.get_sensors(), object_id);
#else
  return nullptr;
#endif
}

#ifdef USE_SENSOR
/// Whether `sensor` can feed a thermostat: its setpoints, band and cut-out are in °C.
inline bool reports_celsius(const sensor::Sensor &sensor) { return sensor.get_unit_of_measurement_ref() == "°C"; }
#endif

inline switch_::Switch *find_switch(const std::string &object_id) {
#ifdef USE_SWITCH
  return find_entity<switch_::Switch>(App.get_switches(), object_id);
#else
  return nullptr;
#endif
}

}  // namespace esphome::climate_hub
