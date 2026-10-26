#pragma once

#include <ArduinoJson.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome::schedules {

/// The file format this firmware writes. A file with a higher one came from a newer firmware:
/// it is read as far as this one understands it and never written back.
static constexpr uint16_t SCHEDULE_VERSION = 1;
/// A file larger than this is refused before it is parsed, so none is ever written.
static constexpr size_t MAX_FILE_BYTES = 16384;
static constexpr size_t MAX_POINTS_PER_DAY = 12;
/// In characters, not bytes: a name may be Cyrillic.
static constexpr size_t NAME_MAX_LENGTH = 48;
/// `<id>.json.tmp` has to stay inside LittleFS's 64-byte names.
static constexpr size_t ID_MAX_LENGTH = 48;

/// What a schedule drives, by object id or by thermostat id.
enum class TargetType : uint8_t { RELAY, THERMOSTAT };
/// What a switch point sets: a relay takes ON or OFF, a thermostat OFF, PRESET or TARGET.
enum class ValueType : uint8_t { OFF, ON, PRESET, TARGET };

/// Why ScheduleConfig::encode() refused.
enum class EncodeError : uint8_t { NONE, TOO_LARGE, NO_MEMORY };

const char *target_type_to_string(TargetType type);
const char *value_type_to_string(ValueType type);

/// From `minute_of_week` on, until the next point, the target takes this value.
struct Point {
  /// Monday 00:00 is 0.
  uint16_t minute_of_week{0};
  ValueType type{ValueType::OFF};
  /// PRESET: the key's index in ScheduleConfig::preset_keys.
  uint8_t preset_index{0};
  /// TARGET: the temperature; the thermostat clamps it into its range.
  float target{NAN};
};

/// The stored document of one schedule, without the JSON: the week is one sorted list.
struct ScheduleConfig {
  /// SCHEDULE_VERSION, or the higher one of a file a newer firmware wrote.
  uint16_t version{SCHEDULE_VERSION};
  std::string id;
  std::string name;
  bool enabled{true};
  TargetType target_type{TargetType::RELAY};
  /// A relay's object id or a thermostat's id.
  std::string target_id;
  /// Sorted by minute_of_week.
  std::vector<Point> points;
  /// The preset keys the points name, each once.
  std::vector<std::string> preset_keys;

  /// Key order is fixed and all seven days are written, so a golden test can compare byte for byte.
  void serialize(JsonObject root) const;
  /// `{"type":"preset","preset":"eco"}` and the like: a point's value as the file holds it.
  void write_value(JsonObject obj, const Point &point) const;
  /// The file's content. Refused, `out` untouched, when the next boot could not load it back:
  /// over `max_bytes`, or cut short by an allocation that failed.
  EncodeError encode(std::string *out, size_t max_bytes = MAX_FILE_BYTES,
                     ArduinoJson::Allocator *allocator = ArduinoJson::detail::DefaultAllocator::instance()) const;

  /// A broken rule is refused with a sentence in `error`, the first one in the editor's order:
  /// the name, the target, then the week a day at a time. The name is trimmed, a missing day
  /// reads as none, each day's points are sorted. An older version reads as this one; a newer
  /// one is kept, so the engine knows not to write the file.
  bool deserialize(JsonObjectConst root, bool require_id, std::string *error);
  /// The rules deserialize() applies, for a document built in C++: sort_points() first.
  bool validate(std::string *error) const;

  void sort_points();
  /// Appends a point after any at the same minute, which validate() refuses; a PRESET's key
  /// joins preset_keys once.
  void add_point(uint16_t minute_of_week, ValueType type, const std::string &preset = "", float target = NAN);
  /// A PRESET point's key, "" for any other.
  const std::string &preset_of(const Point &point) const;

  /// The point whose value holds at `minute_of_week`: the last one at or before it, or, before
  /// the week's first, the week's last. nullptr for no points.
  const Point *in_force(uint16_t minute_of_week) const;
  /// The first point after `minute_of_week`, going round into next week. nullptr for no points.
  const Point *next_after(uint16_t minute_of_week) const;

  /// Written by a newer firmware: this one must not write it back.
  bool from_newer_firmware() const { return this->version > SCHEDULE_VERSION; }
  /// Whether `other` drives the same relay or thermostat: one enabled schedule each.
  bool same_target(const ScheduleConfig &other) const {
    return this->target_type == other.target_type && this->target_id == other.target_id;
  }
};

/// Reduces a name to [a-z0-9-], collapsed and trimmed, at most ID_MAX_LENGTH characters;
/// `fallback` when nothing survives, as of a Cyrillic name.
std::string slugify_id(const std::string &name, const char *fallback = "schedule");
/// `base` with "-<n>" appended, cut so the whole stays within ID_MAX_LENGTH; n < 2 is `base`.
std::string id_with_suffix(const std::string &base, unsigned n);
/// The rules an id read from outside meets: present, and a slug.
bool validate_id(const std::string &id, std::string *error);

/// The name with surrounding ASCII whitespace removed.
std::string trim_name(const std::string &name);
/// The name rules on a trimmed name: 1..NAME_MAX_LENGTH characters of valid UTF-8, no control
/// characters, neither '/' nor '\'.
bool validate_name(const std::string &name, std::string *error);
/// The name as a person reads it: inner whitespace collapsed, ASCII lowercased.
std::string name_key(const std::string &name);
/// At most `chars` characters of valid UTF-8 `text`, never half of one.
std::string cut_chars(const std::string &text, size_t chars);

}  // namespace esphome::schedules
