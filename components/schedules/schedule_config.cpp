#include "schedule_config.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include "week_clock.h"

namespace esphome::schedules {

const char *const COLOR_KEYS[COLOR_COUNT] = {"red",  "orange", "amber",  "yellow", "lime", "green", "teal",
                                             "cyan", "blue",   "indigo", "violet", "pink", "grey"};

namespace {

static_assert(MAX_POINTS_PER_DAY == 12, "the sentences below name the limit");
static_assert(NAME_MAX_LENGTH == 48, "the sentences below name the limit");
static_assert(MAX_MODES == 8 && MAX_ACTIONS == 12, "the sentences below name the limits");
static_assert(MODE_NAME_MAX_LENGTH == 24 && MODE_ID_MAX_LENGTH == 24, "the sentences below name the limits");
static_assert(MIN_WAIT_SECONDS == 1 && MAX_WAIT_SECONDS == 86400, "the sentences below name the limits");
// Indexes fit the uint8_t fields: every action names at most one target and one preset.
static_assert(MAX_MODES * MAX_ACTIONS <= 255, "an Action's indexes are uint8_t");

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

uint8_t color_from_string(const std::string &key) {
  for (size_t i = 0; i < COLOR_COUNT; i++) {
    if (key == COLOR_KEYS[i])
      return static_cast<uint8_t>(i + 1);
  }
  return 0;
}

// "color must be one of red, orange, … pink or grey".
std::string color_rule() {
  std::string out = "color must be one of ";
  for (size_t i = 0; i < COLOR_COUNT; i++)
    out += std::string(i == 0 ? "" : i + 1 == COLOR_COUNT ? " or " : ", ") + COLOR_KEYS[i];
  return out;
}

// "Mon 06:30: ", which heads whatever a point broke.
std::string point_at(uint16_t minute_of_week) {
  return std::string(DAY_TITLES[minute_of_week / MINUTES_PER_DAY]) + " " +
         format_hhmm(minute_of_week % MINUTES_PER_DAY) + ": ";
}

// Decodes the character at `*i` and moves past it; false on anything that is not UTF-8,
// overlong forms and surrogates included.
bool next_char(std::string_view text, size_t *i, uint32_t *out) {
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

// The characters of `text`; false for one that is not UTF-8. `control` says whether a C0, DEL
// or C1 character is among them.
bool count_chars(std::string_view text, size_t *chars, bool *control) {
  *chars = 0;
  *control = false;
  for (size_t i = 0; i < text.size(); (*chars)++) {
    uint32_t c;
    if (!next_char(text, &i, &c))
      return false;
    *control = *control || c < 0x20 || (c >= 0x7F && c <= 0x9F);
  }
  return true;
}

// The name rules of a mode, on a trimmed name, as the end of a sentence about it.
bool check_mode_name(std::string_view name, const std::string &at, std::string *error) {
  if (name.empty())
    return fail(error, at + "name is required");
  size_t chars;
  bool control;
  if (!count_chars(name, &chars, &control))
    return fail(error, at + "name is not valid UTF-8");
  if (chars > MODE_NAME_MAX_LENGTH)
    return fail(error, at + "name is longer than 24 characters");
  if (control)
    return fail(error, at + "name cannot contain control characters");
  return true;
}

bool is_mode_id(std::string_view id) {
  return !id.empty() && id.size() <= MODE_ID_MAX_LENGTH && std::all_of(id.begin(), id.end(), [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
  });
}

bool is_preset_key(std::string_view key) { return key.size() <= ID_MAX_LENGTH && slugify_id(key, "") == key; }

// A wait's seconds: whole, then in range.
bool check_wait(double seconds, const std::string &at, std::string *error) {
  if (!std::isfinite(seconds) || std::floor(seconds) != seconds)
    return fail(error, at + "seconds must be a whole number");
  if (seconds < MIN_WAIT_SECONDS || seconds > MAX_WAIT_SECONDS)
    return fail(error, at + "a wait is 1 s to 24 h");
  return true;
}

// A preset value's key, before it joins the table.
bool check_preset(std::string_view key, const std::string &at, std::string *error) {
  if (key.empty())
    return fail(error, at + "preset is required");
  if (!is_preset_key(key))
    return fail(error, at + "preset must be a preset's key");
  return true;
}

// The mode's sentences start with its name once it has one that holds.
std::string mode_label(std::string_view name) { return "Mode \"" + std::string(name) + "\""; }

bool by_minute(const Point &a, const Point &b) { return a.minute_of_week < b.minute_of_week; }

// One action as a file holds it, appended to the mode added last: its type, what it drives,
// then its value, each checked as it is read.
bool read_action(JsonVariantConst item, const std::string &at, ScheduleConfig *config, std::string *error) {
  if (!item.is<JsonObjectConst>())
    return fail(error, at + "an action must be an object");
  const std::string type = text_of(item["type"]);
  if (type.empty())
    return fail(error, at + "type is required");
  if (type == "wait") {
    JsonVariantConst seconds = item["seconds"];
    if (!seconds.is<double>())
      return fail(error, at + "seconds must be a whole number");
    if (!check_wait(seconds.as<double>(), at, error))
      return false;
    config->add_wait(static_cast<uint32_t>(seconds.as<double>()));
    return true;
  }
  if (type != "relay" && type != "thermostat")
    return fail(error, at + "unknown action type \"" + type + "\"");
  const std::string id = text_of(item["id"]);
  if (id.empty())
    return fail(error, at + "id is required");
  JsonObjectConst value = item["value"].as<JsonObjectConst>();
  const std::string word = text_of(value["type"]);
  // A value without a type says nothing more than none at all.
  if (word.empty())
    return fail(error, at + "value is required");
  ValueType kind;
  if (!value_type_from_string(word, &kind))
    return fail(error, at + "unknown value type \"" + word + "\"");
  if (type == "relay") {
    if (kind != ValueType::ON && kind != ValueType::OFF)
      return fail(error, at + "a relay takes on or off");
    config->add_relay(id, kind == ValueType::ON);
    return true;
  }
  std::string key;
  float target = NAN;
  if (kind == ValueType::PRESET) {
    key = text_of(value["preset"]);
    if (!check_preset(key, at, error))
      return false;
  } else if (kind == ValueType::TARGET) {
    JsonVariantConst number = value["target"];
    target = number.is<double>() ? static_cast<float>(number.as<double>()) : NAN;
    // A double past a float's range turns infinite, which is no target either.
    if (!std::isfinite(target))
      return fail(error, at + "target must be a number");
  }
  config->add_thermostat(id, kind, key, target);
  return true;
}

// One mode, numbered `number` from 1: its name, then its id, its colour and its actions.
bool read_mode(JsonVariantConst item, size_t number, ScheduleConfig *config, std::string *error) {
  const std::string at = "Mode " + std::to_string(number) + ": ";
  if (!item.is<JsonObjectConst>())
    return fail(error, at + "a mode must be an object");
  const std::string name = trim_name(text_of(item["name"]));
  if (!check_mode_name(name, at, error))
    return false;
  const std::string key = name_key(name);
  if (std::any_of(config->modes.begin(), config->modes.end(),
                  [&key](const Mode &other) { return name_key(other.name) == key; }))
    return fail(error, "Two modes are called \"" + name + "\"");
  const std::string label = mode_label(name);
  const std::string id = text_of(item["id"]);
  if (id.empty())
    return fail(error, label + ": id is required");
  if (!is_mode_id(id))
    return fail(error, label + ": id must be at most 24 lowercase letters, digits and dashes");
  if (config->mode_index(id) >= 0)
    return fail(error, "Two modes have the id \"" + id + "\"");
  uint8_t color = 0;
  if (!item["color"].isNull()) {
    color = color_from_string(text_of(item["color"]));
    if (color == 0)
      return fail(error, label + ": " + color_rule());
  }
  JsonVariantConst list = item["actions"];
  if (!list.isNull() && !list.is<JsonArrayConst>())
    return fail(error, label + ": actions must be a list");
  JsonArrayConst items = list.as<JsonArrayConst>();
  if (items.size() > MAX_ACTIONS)
    return fail(error, label + ": at most 12 actions");
  config->add_mode(id, name, color);
  size_t index = 0;
  for (JsonVariantConst action : items) {
    if (!read_action(action, label + ", action " + std::to_string(++index) + ": ", config, error))
      return false;
  }
  return true;
}

// The point's clock, then whether the day has it already, then its mode: the editor's order,
// so a form names the same fault the device does.
bool read_point(JsonVariantConst item, uint8_t day, size_t day_start, const ScheduleConfig &config, Point *out,
                std::string *error) {
  const std::string title = DAY_TITLES[day];
  if (!item.is<JsonObjectConst>())
    return fail(error, title + ": a switch point must be an object");
  uint16_t minute;
  if (!item["at"].is<const char *>() || !parse_hhmm(item["at"].as<const char *>(), &minute))
    return fail(error, title + ": at must be HH:MM from 00:00 to 23:59");
  out->minute_of_week = static_cast<uint16_t>(day * MINUTES_PER_DAY + minute);
  const auto &points = config.points;
  if (std::any_of(points.begin() + day_start, points.end(),
                  [out](const Point &p) { return p.minute_of_week == out->minute_of_week; }))
    return fail(error, title + ": " + format_hhmm(minute) + " is there twice");
  const std::string at = point_at(out->minute_of_week);
  const std::string mode = text_of(item["mode"]);
  if (mode.empty())
    return fail(error, at + "mode is required");
  const int index = config.mode_index(mode);
  if (index < 0)
    return fail(error, at + "there is no mode \"" + mode + "\"");
  out->mode = static_cast<uint8_t>(index);
  return true;
}

// The rules read_action() applies, for an action built in C++.
bool check_action(const ScheduleConfig &config, const Action &action, const std::string &at, std::string *error) {
  if (action.type == ActionType::WAIT)
    return check_wait(action.number, at, error);
  const TargetType want = action.type == ActionType::RELAY ? TargetType::RELAY : TargetType::THERMOSTAT;
  if (action.target >= config.targets.size() || config.targets[action.target].type != want ||
      config.targets[action.target].id.empty())
    return fail(error, at + "id is required");
  if (action.type == ActionType::RELAY) {
    if (action.value != ValueType::ON && action.value != ValueType::OFF)
      return fail(error, at + "a relay takes on or off");
    return true;
  }
  if (action.value == ValueType::PRESET && !check_preset(config.preset_of(action), at, error))
    return false;
  if (action.value == ValueType::TARGET && !std::isfinite(action.number))
    return fail(error, at + "target must be a number");
  return true;
}

}  // namespace

const char *target_type_to_string(TargetType type) { return type == TargetType::RELAY ? "relay" : "thermostat"; }

const char *action_type_to_string(ActionType type) {
  switch (type) {
    case ActionType::RELAY:
      return "relay";
    case ActionType::THERMOSTAT:
      return "thermostat";
    case ActionType::WAIT:
      return "wait";
  }
  return "wait";
}

const char *value_type_to_string(ValueType type) { return VALUE_WORDS[static_cast<uint8_t>(type)]; }

const char *color_to_string(uint8_t color) { return color == 0 || color > COLOR_COUNT ? "" : COLOR_KEYS[color - 1]; }

std::string slugify_id(std::string_view name, const char *fallback) {
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

bool validate_id(std::string_view id, std::string *error) {
  if (id.empty())
    return fail(error, "id is required");
  // The id becomes a path component: a hand-edited file could otherwise send the next save
  // anywhere on the filesystem.
  if (slugify_id(id, "") != id)
    return fail(error, "id must be a slug: lowercase letters, digits and single dashes");
  return true;
}

std::string trim_name(std::string_view name) {
  size_t begin = 0;
  size_t end = name.size();
  while (begin < end && is_ascii_space(name[begin]))
    begin++;
  while (end > begin && is_ascii_space(name[end - 1]))
    end--;
  return std::string(name.substr(begin, end - begin));
}

bool validate_name(std::string_view name, std::string *error) {
  if (name.empty())
    return fail(error, "Name is required");
  size_t chars;
  bool control;
  if (!count_chars(name, &chars, &control))
    return fail(error, "Name is not valid UTF-8");
  if (chars > NAME_MAX_LENGTH)
    return fail(error, "Name is longer than 48 characters");
  if (control)
    return fail(error, "Name cannot contain control characters");
  // A URL segment cannot carry '/', and a browser turns '\' into one.
  if (name.find('/') != std::string_view::npos)
    return fail(error, "Name cannot contain '/'");
  if (name.find('\\') != std::string_view::npos)
    return fail(error, "Name cannot contain '\\'");
  return true;
}

std::string name_key(std::string_view name) {
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

std::string cut_chars(std::string_view text, size_t chars) {
  size_t i = 0;
  for (size_t n = 0; n < chars && i < text.size(); n++) {
    uint32_t c;
    if (!next_char(text, &i, &c))
      break;
  }
  return std::string(text.substr(0, i));
}

void ScheduleConfig::serialize(JsonObject root) const {
  root["version"] = this->version;
  root["revision"] = this->revision;
  root["id"] = this->id;
  root["name"] = this->name;
  root["enabled"] = this->enabled;
  JsonArray modes = root["modes"].to<JsonArray>();
  for (const Mode &mode : this->modes) {
    JsonObject obj = modes.add<JsonObject>();
    obj["id"] = mode.id;
    obj["name"] = mode.name;
    if (mode.color != 0)
      obj["color"] = color_to_string(mode.color);
    JsonArray actions = obj["actions"].to<JsonArray>();
    for (size_t i = 0; i < mode.count; i++)
      this->write_action(actions.add<JsonObject>(), this->action(mode, i));
  }
  JsonObject week = root["week"].to<JsonObject>();
  size_t next = 0;
  for (uint8_t day = 0; day < 7; day++) {
    JsonArray list = week[DAY_NAMES[day]].to<JsonArray>();
    for (; next < this->points.size() && this->points[next].minute_of_week / MINUTES_PER_DAY == day; next++) {
      const Point &point = this->points[next];
      JsonObject obj = list.add<JsonObject>();
      obj["at"] = format_hhmm(point.minute_of_week % MINUTES_PER_DAY);
      obj["mode"] = this->modes[point.mode].id;
    }
  }
}

void ScheduleConfig::write_action(JsonObject obj, const Action &action) const {
  obj["type"] = action_type_to_string(action.type);
  if (action.type == ActionType::WAIT) {
    obj["seconds"] = static_cast<uint32_t>(action.number);
    return;
  }
  obj["id"] = this->target_of(action).id;
  JsonObject value = obj["value"].to<JsonObject>();
  value["type"] = value_type_to_string(action.value);
  if (action.value == ValueType::PRESET) {
    value["preset"] = this->preset_of(action);
  } else if (action.value == ValueType::TARGET) {
    value["target"] = action.number;
  }
}

// A string's own block, none while it fits inside the object; a block costs its header too.
static size_t string_heap(const PsramString &text) {
  static const size_t INLINE = PsramString().capacity();
  return text.capacity() > INLINE ? text.capacity() + 1 + 2 * sizeof(void *) : 0;
}

size_t ScheduleConfig::heap_bytes() const {
  size_t bytes = string_heap(this->id) + string_heap(this->name) + this->modes.capacity() * sizeof(Mode) +
                 this->actions.capacity() * sizeof(Action) + this->targets.capacity() * sizeof(Target) +
                 this->preset_keys.capacity() * sizeof(PsramString) + this->points.capacity() * sizeof(Point);
  for (const Mode &mode : this->modes)
    bytes += string_heap(mode.id) + string_heap(mode.name);
  for (const Target &target : this->targets)
    bytes += string_heap(target.id);
  for (const PsramString &key : this->preset_keys)
    bytes += string_heap(key);
  return bytes;
}

EncodeError ScheduleConfig::encode(JsonDocument &doc, size_t max_bytes) const {
  doc.clear();
  this->serialize(doc.to<JsonObject>());
  // A failed allocation drops members silently; what is left would still serialize.
  if (doc.overflowed())
    return EncodeError::NO_MEMORY;
  if (measureJson(doc) > max_bytes)
    return EncodeError::TOO_LARGE;
  return EncodeError::NONE;
}

EncodeError ScheduleConfig::encode(std::string *out, size_t max_bytes, ArduinoJson::Allocator *allocator) const {
  JsonDocument doc(allocator);
  const EncodeError error = this->encode(doc, max_bytes);
  if (error != EncodeError::NONE)
    return error;
  std::string json;
  serializeJson(doc, json);
  *out = std::move(json);
  return EncodeError::NONE;
}

bool ScheduleConfig::deserialize(JsonObjectConst root, bool require_id, std::string *error) {
  if (root.isNull())
    return fail(error, "document is not an object");
  this->version = read_version(root["version"]);
  this->revision = root["revision"] | 0u;
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

  this->modes.clear();
  this->actions.clear();
  this->targets.clear();
  this->preset_keys.clear();
  this->points.clear();
  JsonVariantConst modes = root["modes"];
  if (!modes.is<JsonArrayConst>())
    return fail(error, "modes is required");
  JsonArrayConst list = modes.as<JsonArrayConst>();
  if (list.size() == 0)
    return fail(error, "A schedule needs at least one mode");
  if (list.size() > MAX_MODES)
    return fail(error, "A schedule has at most 8 modes");
  size_t number = 0;
  for (JsonVariantConst item : list) {
    if (!read_mode(item, ++number, this, error))
      return false;
  }

  JsonObjectConst week = root["week"].as<JsonObjectConst>();
  if (week.isNull())
    return fail(error, "week is required");
  for (JsonPairConst day : week) {
    const char *key = day.key().c_str();
    if (std::none_of(std::begin(DAY_NAMES), std::end(DAY_NAMES),
                     [key](const char *day_name) { return strcmp(day_name, key) == 0; }))
      return fail(error, std::string("Unknown day \"") + key + "\": expected mon, tue, wed, thu, fri, sat or sun");
  }
  for (uint8_t day = 0; day < 7; day++) {
    JsonVariantConst days = week[DAY_NAMES[day]];
    if (days.isNull())
      continue;
    const std::string title = DAY_TITLES[day];
    if (!days.is<JsonArrayConst>())
      return fail(error, title + ": switch points must be a list");
    JsonArrayConst items = days.as<JsonArrayConst>();
    if (items.size() > MAX_POINTS_PER_DAY)
      return fail(error, title + ": at most 12 switch points a day");
    const size_t first = this->points.size();
    for (JsonVariantConst item : items) {
      Point point;
      if (!read_point(item, day, first, *this, &point, error))
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
  if (this->modes.empty())
    return fail(error, "A schedule needs at least one mode");
  if (this->modes.size() > MAX_MODES)
    return fail(error, "A schedule has at most 8 modes");
  for (size_t m = 0; m < this->modes.size(); m++) {
    const Mode &mode = this->modes[m];
    if (!check_mode_name(mode.name, "Mode " + std::to_string(m + 1) + ": ", error))
      return false;
    for (size_t other = 0; other < m; other++) {
      if (name_key(this->modes[other].name) == name_key(mode.name))
        return fail(error, "Two modes are called \"" + std::string(mode.name) + "\"");
    }
    const std::string label = mode_label(mode.name);
    if (mode.id.empty())
      return fail(error, label + ": id is required");
    if (!is_mode_id(mode.id))
      return fail(error, label + ": id must be at most 24 lowercase letters, digits and dashes");
    for (size_t other = 0; other < m; other++) {
      if (this->modes[other].id == mode.id)
        return fail(error, "Two modes have the id \"" + std::string(mode.id) + "\"");
    }
    if (mode.color > COLOR_COUNT)
      return fail(error, label + ": " + color_rule());
    if (mode.count > MAX_ACTIONS)
      return fail(error, label + ": at most 12 actions");
    // Only C++ can place a mode's list outside the actions.
    if (mode.first + mode.count > this->actions.size())
      return fail(error, label + ": actions must be a list");
    for (size_t i = 0; i < mode.count; i++) {
      if (!check_action(*this, this->action(mode, i), label + ", action " + std::to_string(i + 1) + ": ", error))
        return false;
    }
  }
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
    if (point.mode >= this->modes.size())
      return fail(error, point_at(point.minute_of_week) + "mode is required");
  }
  return true;
}

void ScheduleConfig::trim_names() {
  this->name = trim_name(this->name);
  for (Mode &mode : this->modes)
    mode.name = trim_name(mode.name);
}

void ScheduleConfig::sort_points() { std::stable_sort(this->points.begin(), this->points.end(), by_minute); }

void ScheduleConfig::add_mode(std::string_view id, std::string_view name, uint8_t color) {
  Mode mode;
  mode.id = id;
  mode.name = name;
  mode.color = color;
  mode.first = static_cast<uint8_t>(this->actions.size());
  this->modes.push_back(std::move(mode));
}

Action &ScheduleConfig::append_(ActionType type) {
  this->actions.emplace_back();
  Action &action = this->actions.back();
  action.type = type;
  if (!this->modes.empty())
    this->modes.back().count++;
  return action;
}

uint8_t ScheduleConfig::target_index_(TargetType type, std::string_view id) {
  Target target{type, id};
  auto it = std::find(this->targets.begin(), this->targets.end(), target);
  if (it == this->targets.end())
    it = this->targets.insert(it, std::move(target));
  return static_cast<uint8_t>(it - this->targets.begin());
}

uint8_t ScheduleConfig::preset_index_(std::string_view key) {
  auto it = std::find(this->preset_keys.begin(), this->preset_keys.end(), key);
  if (it == this->preset_keys.end())
    it = this->preset_keys.insert(it, PsramString(key));
  return static_cast<uint8_t>(it - this->preset_keys.begin());
}

void ScheduleConfig::add_relay(std::string_view relay, bool on) {
  const uint8_t target = this->target_index_(TargetType::RELAY, relay);
  Action &action = this->append_(ActionType::RELAY);
  action.target = target;
  action.value = on ? ValueType::ON : ValueType::OFF;
}

void ScheduleConfig::add_thermostat(std::string_view thermostat, ValueType value, std::string_view preset,
                                    float target) {
  const uint8_t index = this->target_index_(TargetType::THERMOSTAT, thermostat);
  const uint8_t key = value == ValueType::PRESET ? this->preset_index_(preset) : 0;
  Action &action = this->append_(ActionType::THERMOSTAT);
  action.target = index;
  action.value = value;
  action.preset = key;
  action.number = value == ValueType::TARGET ? target : NAN;
}

void ScheduleConfig::add_wait(uint32_t seconds) {
  this->append_(ActionType::WAIT).number = static_cast<float>(seconds);
}

void ScheduleConfig::add_point(uint16_t minute_of_week, uint8_t mode) {
  Point point;
  point.minute_of_week = minute_of_week;
  point.mode = mode;
  auto at = std::upper_bound(this->points.begin(), this->points.end(), point, by_minute);
  this->points.insert(at, point);
}

const PsramString &ScheduleConfig::preset_of(const Action &action) const {
  static const PsramString NONE;
  if (action.type != ActionType::THERMOSTAT || action.value != ValueType::PRESET ||
      action.preset >= this->preset_keys.size())
    return NONE;
  return this->preset_keys[action.preset];
}

int ScheduleConfig::mode_index(std::string_view id) const {
  for (size_t i = 0; i < this->modes.size(); i++) {
    if (this->modes[i].id == id)
      return static_cast<int>(i);
  }
  return -1;
}

bool ScheduleConfig::names_target(const Target &target) const {
  return std::find(this->targets.begin(), this->targets.end(), target) != this->targets.end();
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
