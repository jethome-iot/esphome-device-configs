#pragma once

// For a suite whose component registers entities at setup and is booted again and again in one
// process: App's entity lists have a fixed size and would soon refuse the entities, so each
// boot starts from the lengths taken here. The entities themselves are never freed, as on a
// device. Listed in a suite's `includes:` next to main.cpp.

#include <algorithm>
#include <cstddef>
#include "esphome/core/application.h"
#include "esphome/core/defines.h"

namespace esphome::testing {

class EntityTableMark {
 public:
  // The lengths now.
  EntityTableMark() {
#ifdef USE_SENSOR
    this->sensors_ = App.get_sensors().size();
#endif
#ifdef USE_BINARY_SENSOR
    this->binary_sensors_ = App.get_binary_sensors().size();
#endif
#ifdef USE_TEXT_SENSOR
    this->text_sensors_ = App.get_text_sensors().size();
#endif
  }

  // Back to those lengths.
  void restore() const {
#ifdef USE_SENSOR
    cut(App.get_sensors(), this->sensors_);
#endif
#ifdef USE_BINARY_SENSOR
    cut(App.get_binary_sensors(), this->binary_sensors_);
#endif
#ifdef USE_TEXT_SENSOR
    cut(App.get_text_sensors(), this->text_sensors_);
#endif
  }

 protected:
  // App hands the lists out const; only a test shortens one.
  template<typename L> static void cut(const L &list, size_t size) {
    auto &writable = const_cast<L &>(list);  // NOLINT(cppcoreguidelines-pro-type-const-cast)
    writable.assign(list.begin(), list.begin() + std::min(size, list.size()));
  }

  size_t sensors_{0};
  size_t binary_sensors_{0};
  size_t text_sensors_{0};
};

}  // namespace esphome::testing
