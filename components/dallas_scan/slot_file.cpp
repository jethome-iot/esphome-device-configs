#include "slot_file.h"
#ifdef USE_DALLAS_SCAN_FILE

#include <algorithm>
#include <cinttypes>

#include "dallas_scan.h"

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
  this->parse_offsets_(root);
  return true;
}

// Apart from the records, so older firmware still reads the table; a bad list or entry costs the
// offsets, never the table.
void SlotFile::parse_offsets_(JsonObject root) {
  std::fill(this->offsets_.begin(), this->offsets_.end(), 0);
  if (root["offsets"].isNull())
    return;
  if (!root["offsets"].is<JsonArray>()) {
    ESP_LOGW(TAG, "'offsets' is not an array, ignoring it");
    return;
  }
  JsonArray offsets = root["offsets"];
  std::vector<bool> seen(this->offsets_.size(), false);
  for (JsonObject entry : offsets) {
    const int slot = entry["slot"] | 0;
    if (!entry["slot"].is<int>() || slot < 1) {
      ESP_LOGW(TAG, "An offset without a valid slot, skipping");
      continue;
    }
    if ((size_t) slot > this->offsets_.size()) {
      ESP_LOGW(TAG, "Slot %d's offset is past max_sensors, skipping", slot);
      continue;
    }
    int16_t tenths = 0;
    if (!entry["offset"].is<double>() || !offset_tenths(entry["offset"].as<double>(), tenths)) {
      ESP_LOGW(TAG, "Slot %d: the offset is not a number within ±%.1f, skipping", slot, DallasScan::MAX_OFFSET);
      continue;
    }
    if (seen[slot - 1])
      ESP_LOGW(TAG, "Slot %d's offset is listed twice, keeping the last one", slot);
    seen[slot - 1] = true;
    this->offsets_[slot - 1] = tenths;
  }
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
  // Left out with none, so a file without offsets stays as older firmware wrote it.
  if (std::all_of(this->offsets_.begin(), this->offsets_.end(), [](int16_t tenths) { return tenths == 0; }))
    return;
  JsonArray offsets = root["offsets"].to<JsonArray>();
  for (size_t slot = 0; slot < this->offsets_.size(); slot++) {
    if (this->offsets_[slot] == 0)
      continue;
    JsonObject entry = offsets.add<JsonObject>();
    entry["slot"] = slot + 1;
    // As text, so -0.3 is not written as the float nearest to it.
    entry["offset"] = serialized(str_sprintf("%.1f", this->offsets_[slot] / 10.0));
  }
}

size_t SlotFile::size() {
  return std::count_if(this->table_.begin(), this->table_.end(), [](uint64_t address) { return address != 0; });
}

}  // namespace esphome::dallas_scan

#endif  // USE_DALLAS_SCAN_FILE
