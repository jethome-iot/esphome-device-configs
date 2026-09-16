#pragma once

#include <string>
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/entity_base.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SENSOR
#include "esphome/components/sensor/sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

// A platform with no configured entity has no registry in App; the names must still exist.
namespace esphome::binary_sensor {
class BinarySensor;
}
namespace esphome::sensor {
class Sensor;
}
namespace esphome::switch_ {
class Switch;
}

namespace esphome::automations {

inline std::string object_id_of(const EntityBase &entity) {
  char buf[OBJECT_ID_MAX_LEN];
  return std::string(entity.get_object_id_to(buf));
}

template<typename T, typename V> T *find_entity_by_key(const V &entities, uint32_t key) {
  for (auto *entity : entities) {
    if (entity->get_object_id_hash() == key && !entity->is_internal())
      return entity;
  }
  return nullptr;
}

inline binary_sensor::BinarySensor *find_binary_sensor(uint32_t key) {
#ifdef USE_BINARY_SENSOR
  return find_entity_by_key<binary_sensor::BinarySensor>(App.get_binary_sensors(), key);
#else
  return nullptr;
#endif
}

inline sensor::Sensor *find_sensor(uint32_t key) {
#ifdef USE_SENSOR
  return find_entity_by_key<sensor::Sensor>(App.get_sensors(), key);
#else
  return nullptr;
#endif
}

inline switch_::Switch *find_switch(uint32_t key) {
#ifdef USE_SWITCH
  return find_entity_by_key<switch_::Switch>(App.get_switches(), key);
#else
  return nullptr;
#endif
}

inline std::string binary_sensor_object_id(uint32_t key) {
#ifdef USE_BINARY_SENSOR
  auto *entity = find_binary_sensor(key);
  return entity == nullptr ? "" : object_id_of(*entity);
#else
  return "";
#endif
}

inline std::string sensor_object_id(uint32_t key) {
#ifdef USE_SENSOR
  auto *entity = find_sensor(key);
  return entity == nullptr ? "" : object_id_of(*entity);
#else
  return "";
#endif
}

inline std::string switch_object_id(uint32_t key) {
#ifdef USE_SWITCH
  auto *entity = find_switch(key);
  return entity == nullptr ? "" : object_id_of(*entity);
#else
  return "";
#endif
}

}  // namespace esphome::automations
