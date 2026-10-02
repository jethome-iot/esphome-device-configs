#include "slot_file.h"
#ifdef USE_DALLAS_SCAN_FILE

#include <algorithm>
#include <cinttypes>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::dallas_scan {

static const char *const TAG = "dallas_scan.file";

bool parse_address(const char *text, uint64_t &address) {
  if (text == nullptr || text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
    return false;
  uint64_t value = 0;
  size_t digits = 0;
  for (const char *c = text + 2; *c != '\0'; c++, digits++) {
    const int nibble = *c >= '0' && *c <= '9'   ? *c - '0'
                       : *c >= 'a' && *c <= 'f' ? *c - 'a' + 10
                       : *c >= 'A' && *c <= 'F' ? *c - 'A' + 10
                                                : -1;
    if (nibble < 0 || digits == 16)
      return false;
    value = (value << 4) | (uint64_t) nibble;
  }
  if (digits == 0 || value == 0)
    return false;
  address = value;
  return true;
}

bool SlotFile::parse_json(JsonObject root, uint32_t version) {
  std::fill(this->table_.begin(), this->table_.end(), 0);
  if (!root["records"].is<JsonArray>()) {
    ESP_LOGE(TAG, "'records' must be an array");
    return false;
  }
  JsonArray records = root["records"];
  for (JsonObject record : records) {
    const int slot = record["slot"] | 0;
    if (!record["slot"].is<int>() || slot < 1) {
      ESP_LOGW(TAG, "A record without a valid slot, skipping");
      continue;
    }
    // A smaller max_sensors: the record waits in the file until the slot is back.
    if ((size_t) slot > this->table_.size()) {
      ESP_LOGW(TAG, "Slot %d is past max_sensors, skipping", slot);
      continue;
    }
    uint64_t address = 0;
    if (!parse_address(record["address"] | (const char *) nullptr, address)) {
      ESP_LOGW(TAG, "Slot %d: the address is not a \"0x...\" hex string, skipping", slot);
      continue;
    }
    // Of two records that clash, the later one wins.
    auto earlier = std::find(this->table_.begin(), this->table_.end(), address);
    if (earlier != this->table_.end()) {
      ESP_LOGW(TAG, "0x%016" PRIx64 " is listed twice, keeping slot %d", address, slot);
      *earlier = 0;
    }
    if (this->table_[slot - 1] != 0)
      ESP_LOGW(TAG, "Slot %d is listed twice, keeping the last record", slot);
    this->table_[slot - 1] = address;
  }
  return true;
}

void SlotFile::write_json(JsonObject root, uint32_t version) {
  JsonArray records = root["records"].to<JsonArray>();
  for (size_t slot = 0; slot < this->table_.size(); slot++) {
    if (this->table_[slot] == 0)
      continue;
    JsonObject record = records.add<JsonObject>();
    record["slot"] = slot + 1;
    record["address"] = str_sprintf("0x%016" PRIx64, this->table_[slot]);
  }
}

size_t SlotFile::size() {
  return std::count_if(this->table_.begin(), this->table_.end(), [](uint64_t address) { return address != 0; });
}

}  // namespace esphome::dallas_scan

#endif  // USE_DALLAS_SCAN_FILE
