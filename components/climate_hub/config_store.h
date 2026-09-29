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

  /// Appends "-2", "-3"… until the id is free, within ID_MAX_LENGTH.
  std::string unique_id_from(const std::string &base) const;

  /// Whether another controller already answers to this name, compared by name_key().
  /// `exclude_id` is the one being saved, which never blocks itself.
  bool is_name_taken(const std::string &name, const std::string &exclude_id = "") const;

 protected:
  std::vector<std::unique_ptr<ClimateConfig>> configs_;
};

/// `base` with "-<n>" appended, cut so the whole stays within ID_MAX_LENGTH; n < 2 is `base`.
std::string id_with_suffix(const std::string &base, unsigned n);

}  // namespace esphome::climate_hub
