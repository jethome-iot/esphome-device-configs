#pragma once

#include <memory>
#include <string>
#include <vector>
#include "climate_config.h"

namespace esphome::climate_hub {

/// Owns the loaded documents. Held by unique_ptr, not by value: a running controller keeps a
/// ClimateConfig * for as long as it runs, and a vector<ClimateConfig> would move them all on
/// the next insert.
class ConfigStore {
 public:
  ClimateConfig *add(const ClimateConfig &config);
  ClimateConfig *get(const std::string &id);
  const ClimateConfig *get(const std::string &id) const;
  bool remove(const std::string &id);

  const std::vector<std::unique_ptr<ClimateConfig>> &all() const { return this->configs_; }
  size_t size() const { return this->configs_.size(); }
  bool empty() const { return this->configs_.empty(); }
  void clear() { this->configs_.clear(); }

  /// readdir order is filesystem-dependent; the list is kept sorted by id instead.
  void sort_by_id();

 protected:
  std::vector<std::unique_ptr<ClimateConfig>> configs_;
};

}  // namespace esphome::climate_hub
