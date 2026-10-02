#include "slot_config.h"
#include <algorithm>
#include <cinttypes>
#include "esphome/components/mqtt_config/mqtt_record.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::mqtt_subscriptions {

static const char *const TAG = "mqtt_subscriptions";

static const char *const KIND_MESSAGE = "'kind' must be 'sensor', 'binary_sensor' or 'text_sensor'";
static const char *const DECIMALS_MESSAGE = "'decimals' must be a whole number from 0 to 4";

const char *kind_key(SlotKind kind) {
  switch (kind) {
    case SlotKind::BINARY_SENSOR:
      return "binary_sensor";
    case SlotKind::TEXT_SENSOR:
      return "text_sensor";
    case SlotKind::SENSOR:
    default:
      return "sensor";
  }
}

static bool parse_kind(const std::string &text, SlotKind &kind) {
  for (SlotKind candidate : {SlotKind::SENSOR, SlotKind::BINARY_SENSOR, SlotKind::TEXT_SENSOR}) {
    if (text == kind_key(candidate)) {
      kind = candidate;
      return true;
    }
  }
  return false;
}

static char to_lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c; }

static bool is_ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

std::string trim_ascii(const std::string &text) {
  size_t begin = 0, end = text.size();
  while (begin < end && is_ascii_space(text[begin]))
    begin++;
  while (end > begin && is_ascii_space(text[end - 1]))
    end--;
  return text.substr(begin, end - begin);
}

void SlotConfig::normalize() {
  if (this->topic.empty()) {
    *this = SlotConfig{};
    return;
  }
  if (this->kind != SlotKind::SENSOR) {
    this->unit.clear();
    this->decimals = DEFAULT_DECIMALS;
  }
  if (this->kind != SlotKind::BINARY_SENSOR) {
    this->payload_on = "ON";
    this->payload_off = "OFF";
  }
}

bool SlotConfig::operator==(const SlotConfig &other) const {
  return this->enabled == other.enabled && this->name == other.name && this->topic == other.topic &&
         this->kind == other.kind && this->json_path == other.json_path && this->unit == other.unit &&
         this->decimals == other.decimals && this->payload_on == other.payload_on &&
         this->payload_off == other.payload_off;
}

void SlotConfig::to_json(JsonObject obj) const {
  obj["enabled"] = this->enabled;
  obj["name"] = this->name;
  obj["topic"] = this->topic;
  obj["kind"] = kind_key(this->kind);
  obj["json_path"] = this->json_path;
  obj["unit"] = this->unit;
  obj["decimals"] = this->decimals;
  obj["payload_on"] = this->payload_on;
  obj["payload_off"] = this->payload_off;
}

std::string read_slot(JsonObjectConst obj, Source source, SlotConfig &out) {
  SlotConfig slot;
  bool has_enabled = false;
  bool has_kind = false;
  for (JsonPairConst kv : obj) {
    const std::string key(kv.key().c_str(), kv.key().size());
    JsonVariantConst value = kv.value();
    // The slot number is the caller's; the rest is what a GET answers and the dashboard may echo.
    if (key == "slot" || key == "pending" || key == "status" || key == "entity")
      continue;
    std::string *text = nullptr;
    if (key == "name") {
      text = &slot.name;
    } else if (key == "topic") {
      text = &slot.topic;
    } else if (key == "json_path") {
      text = &slot.json_path;
    } else if (key == "unit") {
      text = &slot.unit;
    } else if (key == "payload_on") {
      text = &slot.payload_on;
    } else if (key == "payload_off") {
      text = &slot.payload_off;
    }
    if (text != nullptr) {
      if (!value.is<const char *>())
        return "'" + key + "' must be a string";
      // Straight to std::string: a NUL would end a C string early, and validation refuses it.
      *text = value.as<std::string>();
    } else if (key == "enabled") {
      if (!value.is<bool>())
        return "'enabled' must be true or false";
      slot.enabled = value.as<bool>();
      has_enabled = true;
    } else if (key == "kind") {
      if (!value.is<const char *>() || !parse_kind(value.as<std::string>(), slot.kind))
        return KIND_MESSAGE;
      has_kind = true;
    } else if (key == "decimals") {
      // int64_t, so 260 is refused for its range rather than read as a wrapped 4.
      if (!value.is<int64_t>())
        return DECIMALS_MESSAGE;
      slot.decimals = value.as<int64_t>();
    } else if (source == Source::BODY) {
      return "'" + key + "' is not a slot field";
    }
  }
  if (source == Source::BODY) {
    if (!has_enabled)
      return "'enabled' must be true or false";
    if (!has_kind)
      return KIND_MESSAGE;
  }
  slot.name = trim_ascii(slot.name);
  out = std::move(slot);
  return "";
}

bool json_path_valid(const std::string &path) {
  size_t segments = 1;
  bool segment_empty = true;
  for (char c : path) {
    if (c == '.') {
      if (segment_empty)
        return false;
      segments++;
      segment_empty = true;
    } else {
      segment_empty = false;
    }
  }
  return !segment_empty && segments <= JSON_PATH_SEGMENTS;
}

static bool payload_state_fits(const std::string &value) { return !value.empty() && value.size() <= PAYLOAD_STATE_MAX; }

bool equal_ignoring_case(const std::string &a, const std::string &b) {
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return to_lower(x) == to_lower(y); });
}

std::string validate_slot(const SlotConfig &slot, const std::vector<std::string> &units) {
  if (slot.name.empty())
    return "'name' is required";
  if (slot.name.size() > SLOT_NAME_MAX)
    return "'name' is over 32 bytes";
  if (!mqtt_config::is_text(slot.name))
    return "'name' must be text without control characters";
  // web_server addresses an entity as <domain>/<name>, and upstream refuses a '/' in names.
  if (slot.name.find('/') != std::string::npos)
    return "'name' cannot contain '/'";

  const std::string &topic = slot.topic;
  if (topic.empty())
    return "'topic' is required";
  if (topic.size() > TOPIC_MAX)
    return "'topic' is over 128 bytes";
  if (!mqtt_config::is_text(topic))
    return "'topic' must be text without control characters";
  if (topic.front() == ' ' || topic.back() == ' ')
    return "'topic' cannot start or end with a space";
  if (topic.find_first_of("+#") != std::string::npos)
    return "'topic' cannot contain '+' or '#': a slot takes one topic";

  if (!slot.json_path.empty()) {
    if (slot.json_path.size() > JSON_PATH_MAX)
      return "'json_path' is over 64 bytes";
    if (!mqtt_config::is_text(slot.json_path))
      return "'json_path' must be text without control characters";
    if (!json_path_valid(slot.json_path))
      return "'json_path' must be up to 6 keys separated by '.'";
  }

  if (slot.kind == SlotKind::SENSOR) {
    if (!slot.unit.empty() && std::find(units.begin(), units.end(), slot.unit) == units.end())
      return "'unit' is not one this firmware offers";
    if (slot.decimals < 0 || slot.decimals > DECIMALS_MAX)
      return DECIMALS_MESSAGE;
  }

  if (slot.kind == SlotKind::BINARY_SENSOR) {
    // Whole literals, as every fixed message here, so the dashboard can match them verbatim.
    if (!payload_state_fits(slot.payload_on))
      return "'payload_on' must be 1 to 32 bytes";
    if (!mqtt_config::is_text(slot.payload_on))
      return "'payload_on' must be text without control characters";
    if (!payload_state_fits(slot.payload_off))
      return "'payload_off' must be 1 to 32 bytes";
    if (!mqtt_config::is_text(slot.payload_off))
      return "'payload_off' must be text without control characters";
    if (equal_ignoring_case(slot.payload_on, slot.payload_off))
      return "'payload_on' and 'payload_off' must differ";
  }
  return "";
}

std::string object_id_of(const std::string &name) {
  std::string id(name.size(), '_');
  std::transform(name.begin(), name.end(), id.begin(), [](char c) { return to_sanitized_char(to_snake_case_char(c)); });
  return id;
}

SlotFile::Status parse_failure_status(DeserializationError error) {
  return error == DeserializationError::NoMemory ? SlotFile::Status::FAILED : SlotFile::Status::UNREADABLE;
}

SlotFile parse_file(const char *data, size_t len, size_t max_slots) {
  SlotFile out;
  out.slots.assign(max_slots, SlotConfig{});
  out.status = SlotFile::Status::UNREADABLE;
  if (len > FILE_MAX)
    return out;
  Document document;
  if (const DeserializationError error = deserializeJson(document.doc, data, len); error) {
    out.status = parse_failure_status(error);
    return out;
  }
  JsonObjectConst root = document.doc.as<JsonObjectConst>();
  JsonVariantConst version = root["version"];
  if (root.isNull() || !version.is<int64_t>() || version.as<int64_t>() < 1)
    return out;
  if (version.as<int64_t>() > FILE_VERSION) {
    out.status = SlotFile::Status::NEWER;
    return out;
  }
  JsonVariantConst slots = root["slots"];
  if (!slots.is<JsonArrayConst>())
    return out;

  std::vector<bool> seen(max_slots, false);
  for (JsonVariantConst entry : slots.as<JsonArrayConst>()) {
    JsonObjectConst obj = entry.as<JsonObjectConst>();
    JsonVariantConst number = obj["slot"];
    if (obj.isNull() || !number.is<int64_t>()) {
      ESP_LOGW(TAG, "A slot entry without a slot number was dropped");
      continue;
    }
    const int64_t slot_number = number.as<int64_t>();
    if (slot_number < 1 || slot_number > static_cast<int64_t>(max_slots)) {
      ESP_LOGW(TAG, "Slot %" PRId64 " is past the %u this firmware has; it goes at the next save", slot_number,
               static_cast<unsigned>(max_slots));
      continue;
    }
    const size_t index = static_cast<size_t>(slot_number - 1);
    if (seen[index]) {
      ESP_LOGW(TAG, "Slot %" PRId64 " is listed twice; the first entry stands", slot_number);
      continue;
    }
    SlotConfig slot;
    if (std::string error = read_slot(obj, Source::FILE, slot); !error.empty()) {
      ESP_LOGW(TAG, "Slot %" PRId64 " was dropped: %s", slot_number, error.c_str());
      continue;
    }
    slot.normalize();
    out.slots[index] = std::move(slot);
    seen[index] = true;
  }
  out.status = SlotFile::Status::OK;
  return out;
}

bool serialize_file(const std::vector<SlotConfig> &slots, std::string &out) {
  Document document;
  return serialize_into(document.doc, slots, out);
}

bool serialize_into(JsonDocument &doc, const std::vector<SlotConfig> &slots, std::string &out) {
  JsonObject root = doc.to<JsonObject>();
  root["version"] = FILE_VERSION;
  JsonArray list = root["slots"].to<JsonArray>();
  for (size_t i = 0; i < slots.size(); i++) {
    if (slots[i].empty())
      continue;
    JsonObject obj = list.add<JsonObject>();
    obj["slot"] = i + 1;
    slots[i].to_json(obj);
  }
  if (doc.overflowed())
    return false;
  out.clear();
  serializeJson(doc, out);
  return true;
}

}  // namespace esphome::mqtt_subscriptions
