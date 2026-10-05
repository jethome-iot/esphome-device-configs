#pragma once

#include "esphome/core/defines.h"
#ifdef USE_DALLAS_SCAN_FILE

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "esphome/components/config_json/settings_base_json.h"

namespace esphome::dallas_scan {

/// The slot table as <key>.json: {"version": 1, "records": [{"slot": 1, "address": "0x28..."}],
/// "offsets": [{"slot": 1, "offset": -0.3}]}.
class SlotFile : public config_json::SettingsBaseJson {
 public:
  SlotFile(std::string key, size_t slots) : key_(std::move(key)), table_(slots, 0), offsets_(slots, 0) {}

  const char *get_key() override { return this->key_.c_str(); }
  bool parse_json(JsonObject root, uint32_t version) override;
  void write_json(JsonObject root, uint32_t version) override;
  // DallasScan loads and saves the file itself; a keeper never holds it.
  void apply() override {}
  size_t size() override;
  void reset() override {
    std::fill(this->table_.begin(), this->table_.end(), 0);
    std::fill(this->offsets_.begin(), this->offsets_.end(), 0);
  }

  /// Slot -> ROM address, 0 = empty.
  const std::vector<uint64_t> &table() const { return this->table_; }
  void set_table(const std::vector<uint64_t> &table) { this->table_ = table; }
  /// Slot -> offset in tenths of a degree, 0 = none.
  const std::vector<int16_t> &offsets() const { return this->offsets_; }
  void set_offsets(const std::vector<int16_t> &offsets) { this->offsets_ = offsets; }

 protected:
  void parse_offsets_(JsonObject root);

  std::string key_;
  std::vector<uint64_t> table_;
  std::vector<int16_t> offsets_;
};

/// "0x" and 1-16 hex digits; false for anything else, and for 0.
bool parse_address(const char *text, uint64_t &address);

}  // namespace esphome::dallas_scan

#endif  // USE_DALLAS_SCAN_FILE
