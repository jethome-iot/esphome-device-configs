#include "input_file.h"

#include <algorithm>

#include "esphome/core/entity_base.h"
#include "esphome/core/log.h"

namespace esphome::virtual_inputs {

static const char *const TAG = "virtual_inputs.file";

bool parse_delay(JsonVariantConst value, uint32_t &seconds) {
  if (!value.is<uint32_t>() || value.as<uint32_t>() > MAX_DELAY_S)
    return false;
  seconds = value.as<uint32_t>();
  return true;
}

bool InputFile::parse_json(JsonObject root, uint32_t version) {
  std::fill(this->table_.begin(), this->table_.end(), InputConfig{});
  if (!root["records"].is<JsonArray>()) {
    ESP_LOGE(TAG, "'records' must be an array");
    return false;
  }
  JsonArray records = root["records"];
  std::vector<bool> seen(this->table_.size(), false);
  for (JsonObject record : records) {
    const int slot = record["slot"] | 0;
    if (!record["slot"].is<int>() || slot < 1) {
      ESP_LOGW(TAG, "A record without a valid slot, skipping");
      continue;
    }
    // A smaller max_inputs: the record waits in the file until the slot is back or a save drops it.
    if (static_cast<size_t>(slot) > this->table_.size()) {
      ESP_LOGW(TAG, "Slot %d is past max_inputs, skipping", slot);
      continue;
    }
    JsonVariant source = record["source"];
    if (!source.is<const char *>() || source.as<JsonString>().size() == 0 ||
        source.as<JsonString>().size() >= OBJECT_ID_MAX_LEN) {
      ESP_LOGW(TAG, "Slot %d: the source is not an object id, skipping", slot);
      continue;
    }
    InputConfig config;
    config.source = source.as<std::string>();
    if ((!record["on_delay"].isNull() && !parse_delay(record["on_delay"], config.on_delay)) ||
        (!record["off_delay"].isNull() && !parse_delay(record["off_delay"], config.off_delay))) {
      ESP_LOGW(TAG, "Slot %d: a delay is not a whole number of seconds up to %u, skipping", slot,
               static_cast<unsigned>(MAX_DELAY_S));
      continue;
    }
    if (seen[slot - 1])
      ESP_LOGW(TAG, "Slot %d is listed twice, keeping the last record", slot);
    seen[slot - 1] = true;
    this->table_[slot - 1] = std::move(config);
  }
  return true;
}

void InputFile::write_json(JsonObject root, uint32_t version) {
  JsonArray records = root["records"].to<JsonArray>();
  for (size_t slot = 0; slot < this->table_.size(); slot++) {
    const InputConfig &config = this->table_[slot];
    if (!config.configured())
      continue;
    JsonObject record = records.add<JsonObject>();
    record["slot"] = slot + 1;
    record["source"] = config.source;
    record["on_delay"] = config.on_delay;
    record["off_delay"] = config.off_delay;
  }
}

size_t InputFile::size() {
  return std::count_if(this->table_.begin(), this->table_.end(),
                       [](const InputConfig &config) { return config.configured(); });
}

}  // namespace esphome::virtual_inputs
