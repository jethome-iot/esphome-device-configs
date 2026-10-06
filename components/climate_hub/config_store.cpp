#include "config_store.h"
#include <algorithm>

namespace esphome::climate_hub {

ClimateConfig *ConfigStore::add(const ClimateConfig &config) {
  this->configs_.push_back(std::make_unique<ClimateConfig>(config));
  return this->configs_.back().get();
}

ClimateConfig *ConfigStore::get(const std::string &id) {
  for (auto &cfg : this->configs_) {
    if (cfg->id == id)
      return cfg.get();
  }
  return nullptr;
}

const ClimateConfig *ConfigStore::get(const std::string &id) const {
  for (const auto &cfg : this->configs_) {
    if (cfg->id == id)
      return cfg.get();
  }
  return nullptr;
}

bool ConfigStore::remove(const std::string &id) {
  for (auto it = this->configs_.begin(); it != this->configs_.end(); ++it) {
    if ((*it)->id == id) {
      this->configs_.erase(it);
      return true;
    }
  }
  return false;
}

void ConfigStore::sort_by_id() {
  std::sort(
      this->configs_.begin(), this->configs_.end(),
      [](const std::unique_ptr<ClimateConfig> &a, const std::unique_ptr<ClimateConfig> &b) { return a->id < b->id; });
}

}  // namespace esphome::climate_hub
