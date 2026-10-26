#include "schedule_config.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include "week_clock.h"

namespace esphome::schedules {

namespace {

static_assert(MAX_POINTS_PER_DAY == 12, "the sentences below name the limit");
static_assert(NAME_MAX_LENGTH == 48, "the sentences below name the limit");

bool fail(std::string *error, const std::string &message) {
  if (error != nullptr)
    *error = message;
  return false;
}

// as<std::string>() would read a number or an object as its JSON text.
std::string text_of(JsonVariantConst value) { return value.is<const char *>() ? value.as<std::string>() : ""; }

bool is_ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// Older files read as this version; a newer one keeps its number, rounded up so 1.5 stays newer.
uint16_t read_version(JsonVariantConst value) {
  const double version = value.is<double>() ? value.as<double>() : 0;
  if (!(version > SCHEDULE_VERSION))
    return SCHEDULE_VERSION;
  return static_cast<uint16_t>(std::min(std::ceil(version), 65535.0));
}

// Indexed by ValueType.
const char *const VALUE_WORDS[] = {"off", "on", "preset", "target"};

bool value_type_from_string(const std::string &word, ValueType *out) {
  for (uint8_t i = 0; i < 4; i++) {
    if (word == VALUE_WORDS[i]) {
      *out = static_cast<ValueType>(i);
      return true;
    }
  }
  return false;
}

// "Mon 06:30: ", which heads whatever a point broke.
std::string point_at(uint16_t minute_of_week) {
  return std::string(DAY_TITLES[minute_of_week / MINUTES_PER_DAY]) + " " +
         format_hhmm(minute_of_week % MINUTES_PER_DAY) + ": ";
}

// Decodes the character at `*i` and moves past it; false on anything that is not UTF-8,
// overlong forms and surrogates included.
bool next_char(const std::string &text, size_t *i, uint32_t *out) {
  const auto lead = static_cast<unsigned char>(text[*i]);
  size_t width;
  uint32_t value;
  uint32_t least;
  if (lead < 0x80) {
    *out = lead;
    *i += 1;
    return true;
  }
  if ((lead & 0xE0) == 0xC0) {
    width = 2;
    value = lead & 0x1F;
    least = 0x80;
  } else if ((lead & 0xF0) == 0xE0) {
    width = 3;
    value = lead & 0x0F;
    least = 0x800;
  } else if ((lead & 0xF8) == 0xF0) {
    width = 4;
    value = lead & 0x07;
    least = 0x10000;
  } else {
    return false;
  }
  if (*i + width > text.size())
    return false;
  for (size_t k = 1; k < width; k++) {
    const auto next = static_cast<unsigned char>(text[*i + k]);
    if ((next & 0xC0) != 0x80)
      return false;
    value = (value << 6) | (next & 0x3F);
  }
  if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
    return false;
  *out = value;
  *i += width;
  return true;
}

bool is_preset_key(const std::string &key) { return key.size() <= ID_MAX_LENGTH && slugify_id(key, "") == key; }

// Whether the target takes a value of this type.
bool check_fit(TargetType target, ValueType type, const std::string &at, std::string *error) {
  if (target == TargetType::RELAY && type != ValueType::ON && type != ValueType::OFF)
    return fail(error, at + "a relay takes on or off");
  if (target == TargetType::THERMOSTAT && type == ValueType::ON)
    return fail(error, at + "a thermostat takes a preset, off or a target");
  return true;
}

// The rules read_point() applies to a value, for a point built in C++.
bool check_value(const ScheduleConfig &config, const Point &point, std::string *error) {
  const std::string at = point_at(point.minute_of_week);
  if (!check_fit(config.target_type, point.type, at, error))
    return false;
  if (point.type == ValueType::PRESET) {
    const std::string &key = config.preset_of(point);
    if (key.empty())
      return fail(error, at + "preset is required");
    if (!is_preset_key(key))
      return fail(error, at + "preset must be a preset's key");
  }
  if (point.type == ValueType::TARGET && !std::isfinite(point.target))
    return fail(error, at + "target must be a number");
  return true;
}

// The point's clock, then whether the day has it already, then its value, checked against the
// target read before: the editor's order, so a form names the same fault the device does.
bool read_point(JsonVariantConst item, uint8_t day, size_t day_start, ScheduleConfig *config, Point *out,
                std::string *error) {
  const std::string title = DAY_TITLES[day];
  if (!item.is<JsonObjectConst>())
    return fail(error, title + ": a switch point must be an object");
  uint16_t minute;
  if (!item["at"].is<const char *>() || !parse_hhmm(item["at"].as<const char *>(), &minute))
    return fail(error, title + ": at must be HH:MM from 00:00 to 23:59");
  out->minute_of_week = static_cast<uint16_t>(day * MINUTES_PER_DAY + minute);
  const auto &points = config->points;
  if (std::any_of(points.begin() + day_start, points.end(),
                  [out](const Point &p) { return p.minute_of_week == out->minute_of_week; }))
    return fail(error, title + ": " + format_hhmm(minute) + " is there twice");
  const std::string at = point_at(out->minute_of_week);
  JsonObjectConst value = item["value"].as<JsonObjectConst>();
  const std::string type = text_of(value["type"]);
  // A value without a type says nothing more than none at all.
  if (type.empty())
    return fail(error, at + "value is required");
  if (!value_type_from_string(type, &out->type))
    return fail(error, at + "unknown value type \"" + type + "\"");
  if (!check_fit(config->target_type, out->type, at, error))
    return false;
  if (out->type == ValueType::PRESET) {
    const std::string key = text_of(value["preset"]);
    if (key.empty())
      return fail(error, at + "preset is required");
    if (!is_preset_key(key))
      return fail(error, at + "preset must be a preset's key");
    auto it = std::find(config->preset_keys.begin(), config->preset_keys.end(), key);
    if (it == config->preset_keys.end())
      it = config->preset_keys.insert(it, key);
    out->preset_index = static_cast<uint8_t>(it - config->preset_keys.begin());
  }
  if (out->type == ValueType::TARGET) {
    JsonVariantConst target = value["target"];
    out->target = target.is<double>() ? static_cast<float>(target.as<double>()) : NAN;
    // A double past a float's range turns infinite, which is no target either.
    if (!std::isfinite(out->target))
      return fail(error, at + "target must be a number");
  }
  return true;
}

bool by_minute(const Point &a, const Point &b) { return a.minute_of_week < b.minute_of_week; }

}  // namespace

const char *target_type_to_string(TargetType type) { return type == TargetType::RELAY ? "relay" : "thermostat"; }

const char *value_type_to_string(ValueType type) { return VALUE_WORDS[static_cast<uint8_t>(type)]; }

std::string slugify_id(const std::string &name, const char *fallback) {
  std::string out;
  out.reserve(name.size());
  bool prev_dash = false;
  for (char c : name) {
    auto u = static_cast<unsigned char>(c);
    if (u < 0x80 && std::isalnum(u) != 0) {
      out += static_cast<char>(std::tolower(u));
      prev_dash = false;
    } else if (!prev_dash && !out.empty()) {
      out += '-';
      prev_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  if (out.size() > ID_MAX_LENGTH)
    out.resize(ID_MAX_LENGTH);
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out.empty() ? fallback : out;
}

std::string id_with_suffix(const std::string &base, unsigned n) {
  if (n < 2)
    return base;
  const std::string tail = "-" + std::to_string(n);
  std::string head = base.substr(0, ID_MAX_LENGTH - tail.size());
  // A cut can end on a dash, which would make "--" and fail the slug check on the next boot.
  while (!head.empty() && head.back() == '-')
    head.pop_back();
  return (head.empty() ? std::string("schedule") : head) + tail;
}

bool validate_id(const std::string &id, std::string *error) {
  if (id.empty())
    return fail(error, "id is required");
  // The id becomes a path component: a hand-edited file could otherwise send the next save
  // anywhere on the filesystem.
  if (slugify_id(id, "") != id)
    return fail(error, "id must be a slug: lowercase letters, digits and single dashes");
  return true;
}

std::string trim_name(const std::string &name) {
  size_t begin = 0;
  size_t end = name.size();
  while (begin < end && is_ascii_space(name[begin]))
    begin++;
  while (end > begin && is_ascii_space(name[end - 1]))
    end--;
  return name.substr(begin, end - begin);
}

bool validate_name(const std::string &name, std::string *error) {
  if (name.empty())
    return fail(error, "Name is required");
  size_t chars = 0;
  bool control = false;
  for (size_t i = 0; i < name.size(); chars++) {
    uint32_t c;
    if (!next_char(name, &i, &c))
      return fail(error, "Name is not valid UTF-8");
    control = control || c < 0x20 || (c >= 0x7F && c <= 0x9F);
  }
  if (chars > NAME_MAX_LENGTH)
    return fail(error, "Name is longer than 48 characters");
  if (control)
    return fail(error, "Name cannot contain control characters");
  // A URL segment cannot carry '/', and a browser turns '\' into one.
  if (name.find('/') != std::string::npos)
    return fail(error, "Name cannot contain '/'");
  if (name.find('\\') != std::string::npos)
    return fail(error, "Name cannot contain '\\'");
  return true;
}

std::string name_key(const std::string &name) {
  std::string out;
  out.reserve(name.size());
  bool pending_space = false;
  for (char c : name) {
    auto u = static_cast<unsigned char>(c);
    if (is_ascii_space(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out += ' ';
      pending_space = false;
    }
    out += u < 0x80 ? static_cast<char>(std::tolower(u)) : c;
  }
  return out;
}

std::string cut_chars(const std::string &text, size_t chars) {
  size_t i = 0;
  for (size_t n = 0; n < chars && i < text.size(); n++) {
    uint32_t c;
    if (!next_char(text, &i, &c))
      break;
  }
  return text.substr(0, i);
}

void ScheduleConfig::serialize(JsonObject root) const {
  root["version"] = this->version;
  root["id"] = this->id;
  root["name"] = this->name;
  root["enabled"] = this->enabled;
  JsonObject target = root["target"].to<JsonObject>();
  target["type"] = target_type_to_string(this->target_type);
  target["id"] = this->target_id;
  JsonObject week = root["week"].to<JsonObject>();
  size_t next = 0;
  for (uint8_t day = 0; day < 7; day++) {
    JsonArray list = week[DAY_NAMES[day]].to<JsonArray>();
    for (; next < this->points.size() && this->points[next].minute_of_week / MINUTES_PER_DAY == day; next++) {
      const Point &point = this->points[next];
      JsonObject obj = list.add<JsonObject>();
      obj["at"] = format_hhmm(point.minute_of_week % MINUTES_PER_DAY);
      this->write_value(obj["value"].to<JsonObject>(), point);
    }
  }
}

void ScheduleConfig::write_value(JsonObject obj, const Point &point) const {
  obj["type"] = value_type_to_string(point.type);
  if (point.type == ValueType::PRESET) {
    obj["preset"] = this->preset_of(point);
  } else if (point.type == ValueType::TARGET) {
    obj["target"] = point.target;
  }
}

EncodeError ScheduleConfig::encode(std::string *out, size_t max_bytes, ArduinoJson::Allocator *allocator) const {
  JsonDocument doc(allocator);
  this->serialize(doc.to<JsonObject>());
  // A failed allocation drops members silently; what is left would still serialize.
  if (doc.overflowed())
    return EncodeError::NO_MEMORY;
  if (measureJson(doc) > max_bytes)
    return EncodeError::TOO_LARGE;
  std::string json;
  serializeJson(doc, json);
  *out = std::move(json);
  return EncodeError::NONE;
}

bool ScheduleConfig::deserialize(JsonObjectConst root, bool require_id, std::string *error) {
  if (root.isNull())
    return fail(error, "document is not an object");
  this->version = read_version(root["version"]);
  if (require_id) {
    this->id = text_of(root["id"]);
    if (!validate_id(this->id, error))
      return false;
  } else if (!root["id"].isNull()) {
    this->id = text_of(root["id"]);
  }
  this->name = trim_name(text_of(root["name"]));
  if (!validate_name(this->name, error))
    return false;
  this->enabled = root["enabled"] | this->enabled;

  JsonObjectConst target = root["target"].as<JsonObjectConst>();
  if (target.isNull())
    return fail(error, "target is required");
  const std::string type = text_of(target["type"]);
  if (type == "relay") {
    this->target_type = TargetType::RELAY;
  } else if (type == "thermostat") {
    this->target_type = TargetType::THERMOSTAT;
  } else {
    return fail(error, "target.type must be relay or thermostat");
  }
  this->target_id = text_of(target["id"]);
  if (this->target_id.empty())
    return fail(error, "target.id is required");

  JsonObjectConst week = root["week"].as<JsonObjectConst>();
  if (week.isNull())
    return fail(error, "week is required");
  for (JsonPairConst day : week) {
    const char *key = day.key().c_str();
    if (std::none_of(std::begin(DAY_NAMES), std::end(DAY_NAMES),
                     [key](const char *day_name) { return strcmp(day_name, key) == 0; }))
      return fail(error, std::string("Unknown day \"") + key + "\": expected mon, tue, wed, thu, fri, sat or sun");
  }
  this->points.clear();
  this->preset_keys.clear();
  for (uint8_t day = 0; day < 7; day++) {
    JsonVariantConst list = week[DAY_NAMES[day]];
    if (list.isNull())
      continue;
    const std::string title = DAY_TITLES[day];
    if (!list.is<JsonArrayConst>())
      return fail(error, title + ": switch points must be a list");
    JsonArrayConst items = list.as<JsonArrayConst>();
    if (items.size() > MAX_POINTS_PER_DAY)
      return fail(error, title + ": at most 12 switch points a day");
    const size_t first = this->points.size();
    for (JsonVariantConst item : items) {
      Point point;
      if (!read_point(item, day, first, this, &point, error))
        return false;
      this->points.push_back(point);
    }
    std::stable_sort(this->points.begin() + first, this->points.end(), by_minute);
  }
  if (this->points.empty())
    return fail(error, "A schedule needs at least one switch point");
  return true;
}

bool ScheduleConfig::validate(std::string *error) const {
  if (!validate_name(this->name, error))
    return false;
  if (this->target_id.empty())
    return fail(error, "target.id is required");
  if (this->points.empty())
    return fail(error, "A schedule needs at least one switch point");
  size_t in_day = 0;
  for (size_t i = 0; i < this->points.size(); i++) {
    const Point &point = this->points[i];
    // Only C++ can place one there: the file has no day for it.
    if (point.minute_of_week >= MINUTES_PER_WEEK)
      return fail(error, "A switch point is past Sun 23:59");
    const uint16_t day = point.minute_of_week / MINUTES_PER_DAY;
    const bool same_day = i > 0 && this->points[i - 1].minute_of_week / MINUTES_PER_DAY == day;
    in_day = same_day ? in_day + 1 : 1;
    if (in_day > MAX_POINTS_PER_DAY)
      return fail(error, std::string(DAY_TITLES[day]) + ": at most 12 switch points a day");
    if (same_day && this->points[i - 1].minute_of_week == point.minute_of_week)
      return fail(error, std::string(DAY_TITLES[day]) + ": " + format_hhmm(point.minute_of_week % MINUTES_PER_DAY) +
                             " is there twice");
    if (!check_value(*this, point, error))
      return false;
  }
  return true;
}

void ScheduleConfig::sort_points() { std::stable_sort(this->points.begin(), this->points.end(), by_minute); }

void ScheduleConfig::add_point(uint16_t minute_of_week, ValueType type, const std::string &preset, float target) {
  Point point;
  point.minute_of_week = minute_of_week;
  point.type = type;
  point.target = target;
  if (type == ValueType::PRESET) {
    auto it = std::find(this->preset_keys.begin(), this->preset_keys.end(), preset);
    if (it == this->preset_keys.end())
      it = this->preset_keys.insert(it, preset);
    point.preset_index = static_cast<uint8_t>(it - this->preset_keys.begin());
  }
  auto at = std::upper_bound(this->points.begin(), this->points.end(), point, by_minute);
  this->points.insert(at, point);
}

const std::string &ScheduleConfig::preset_of(const Point &point) const {
  static const std::string NONE;
  if (point.type != ValueType::PRESET || point.preset_index >= this->preset_keys.size())
    return NONE;
  return this->preset_keys[point.preset_index];
}

const Point *ScheduleConfig::in_force(uint16_t minute_of_week) const {
  if (this->points.empty())
    return nullptr;
  auto it = std::upper_bound(this->points.begin(), this->points.end(), minute_of_week,
                             [](uint16_t minute, const Point &point) { return minute < point.minute_of_week; });
  return it == this->points.begin() ? &this->points.back() : &*(it - 1);
}

const Point *ScheduleConfig::next_after(uint16_t minute_of_week) const {
  if (this->points.empty())
    return nullptr;
  auto it = std::upper_bound(this->points.begin(), this->points.end(), minute_of_week,
                             [](uint16_t minute, const Point &point) { return minute < point.minute_of_week; });
  return it == this->points.end() ? &this->points.front() : &*it;
}

}  // namespace esphome::schedules
