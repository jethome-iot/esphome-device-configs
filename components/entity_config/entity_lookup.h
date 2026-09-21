#pragma once

#include <string>
#include "esphome/core/application.h"
#include "esphome/core/defines.h"
#include "esphome/core/entity_base.h"
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

namespace esphome::entity_config {

inline std::string object_id_of(const EntityBase &entity) {
  char buf[OBJECT_ID_MAX_LEN];
  return std::string(entity.get_object_id_to(buf));
}

// Records are keyed by the object_id hash; internal entities are not settable.
template<typename T, typename V> T *find_entity_by_key(const V &entities, uint32_t key) {
  for (auto *entity : entities) {
    if (entity->get_object_id_hash() == key && !entity->is_internal())
      return entity;
  }
  return nullptr;
}

#ifdef USE_SWITCH
inline switch_::Switch *find_switch(uint32_t key) {
  return find_entity_by_key<switch_::Switch>(App.get_switches(), key);
}
#endif

#ifdef USE_BINARY_SENSOR
inline binary_sensor::BinarySensor *find_binary_sensor(uint32_t key) {
  return find_entity_by_key<binary_sensor::BinarySensor>(App.get_binary_sensors(), key);
}
#endif

}  // namespace esphome::entity_config
