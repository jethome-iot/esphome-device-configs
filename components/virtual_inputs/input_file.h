#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "esphome/components/config_json/settings_base_json.h"

namespace esphome::virtual_inputs {

/// The longest on or off delay, in seconds: a day.
static constexpr uint32_t MAX_DELAY_S = 86400;

/// What one slot follows and how; an empty source is a free slot.
struct InputConfig {
  std::string source;
  uint32_t on_delay{0};   // seconds
  uint32_t off_delay{0};  // seconds

  bool configured() const { return !this->source.empty(); }
  bool operator==(const InputConfig &other) const {
    return this->source == other.source && this->on_delay == other.on_delay && this->off_delay == other.off_delay;
  }
  bool operator!=(const InputConfig &other) const { return !(*this == other); }
};

/// A delay as the file and the API take it: a whole number of seconds from 0 to MAX_DELAY_S.
bool parse_delay(JsonVariantConst value, uint32_t &seconds);

/// The slots as <key>.json: {"version": 1, "records": [{"slot": 1, "source": "input_3",
/// "on_delay": 0, "off_delay": 600}]}.
class InputFile : public config_json::SettingsBaseJson {
 public:
  explicit InputFile(std::string key) : key_(std::move(key)) {}

  const char *get_key() override { return this->key_.c_str(); }
  bool parse_json(JsonObject root, uint32_t version) override;
  void write_json(JsonObject root, uint32_t version) override;
  // VirtualInputs loads and saves the file itself; a keeper never holds it.
  void apply() override {}
  size_t size() override;
  void reset() override { this->table_.assign(this->table_.size(), InputConfig{}); }

  /// Slot -> what it follows; sized by max_inputs.
  const std::vector<InputConfig> &table() const { return this->table_; }
  void set_table(const std::vector<InputConfig> &table) { this->table_ = table; }
  void set_slots(size_t slots) { this->table_.assign(slots, InputConfig{}); }

 protected:
  std::string key_;
  std::vector<InputConfig> table_;
};

}  // namespace esphome::virtual_inputs
