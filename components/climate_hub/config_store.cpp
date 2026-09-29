#include "config_store.h"
#include <algorithm>

namespace esphome::climate_hub {

std::string id_with_suffix(const std::string &base, unsigned n) {
  if (n < 2)
    return base;
  const std::string tail = "-" + std::to_string(n);
  std::string head = base.substr(0, ID_MAX_LENGTH - tail.size());
  // A cut can end on a dash, which would make "--" and fail the slug check on the next boot.
  while (!head.empty() && head.back() == '-')
    head.pop_back();
  return (head.empty() ? std::string("climate") : head) + tail;
}

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

std::string ConfigStore::unique_id_from(const std::string &base) const {
  for (unsigned n = 1; n < 1000; n++) {
    std::string candidate = id_with_suffix(base, n);
    if (this->get(candidate) == nullptr)
      return candidate;
  }
  return id_with_suffix(base, 1000);
}

bool ConfigStore::is_name_taken(const std::string &name, const std::string &exclude_id) const {
  const std::string wanted = name_key(name);
  for (const auto &config : this->configs_) {
    if (config->id == exclude_id)
      continue;
    if (name_key(config->name) == wanted)
      return true;
  }
  return false;
}

}  // namespace esphome::climate_hub
