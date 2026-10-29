#pragma once

#include <ArduinoJson.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "schedule_memory.h"

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
static constexpr size_t MAX_MODES = 8;
static constexpr size_t MAX_ACTIONS = 12;
/// In characters, as NAME_MAX_LENGTH.
static constexpr size_t MODE_NAME_MAX_LENGTH = 24;
static constexpr size_t MODE_ID_MAX_LENGTH = 24;
static constexpr uint32_t MIN_WAIT_SECONDS = 1;
static constexpr uint32_t MAX_WAIT_SECONDS = 86400;

/// The palette a mode's colour is one of; the dashboard maps each key to a colour.
static constexpr size_t COLOR_COUNT = 13;
extern const char *const COLOR_KEYS[COLOR_COUNT];

/// What an action drives, by object id or by thermostat id.
enum class TargetType : uint8_t { RELAY, THERMOSTAT };
enum class ActionType : uint8_t { RELAY, THERMOSTAT, WAIT };
/// What an action sets: a relay ON or OFF, a thermostat any of the four.
enum class ValueType : uint8_t { OFF, ON, PRESET, TARGET };

/// Why ScheduleConfig::encode() refused.
enum class EncodeError : uint8_t { NONE, TOO_LARGE, NO_MEMORY };

const char *target_type_to_string(TargetType type);
const char *action_type_to_string(ActionType type);
const char *value_type_to_string(ValueType type);
/// "" for 0, no colour.
const char *color_to_string(uint8_t color);

/// A relay or a thermostat the schedule's actions name, kept once per schedule.
struct Target {
  TargetType type{TargetType::RELAY};
  PsramString id;
  Target() = default;
  Target(TargetType type, std::string_view id) : type(type), id(id) {}
  bool operator==(const Target &other) const { return this->type == other.type && this->id == other.id; }
};

/// One step of a mode's list. Small on purpose: up to 16 x 8 x 12 of them stay in RAM.
struct Action {
  ActionType type{ActionType::WAIT};
  ValueType value{ValueType::OFF};
  /// RELAY, THERMOSTAT: the index of what it drives in ScheduleConfig::targets.
  uint8_t target{0};
  /// PRESET: the key's index in ScheduleConfig::preset_keys.
  uint8_t preset{0};
  /// TARGET: the temperature, which the thermostat clamps into its range. WAIT: the seconds,
  /// whole, which a float holds exactly up to MAX_WAIT_SECONDS.
  float number{NAN};

  bool drives() const { return this->type != ActionType::WAIT; }
};

/// A named list of actions a switch point runs; its actions are `count` from `first` in
/// ScheduleConfig::actions.
struct Mode {
  /// [a-z0-9-], unique in the schedule; points name the mode by it, so a rename keeps it.
  PsramString id;
  PsramString name;
  /// 0 for none, else one more than its index in COLOR_KEYS.
  uint8_t color{0};
  uint8_t first{0};
  uint8_t count{0};
};

/// From `minute_of_week` on, until the next point, the mode's list has run.
struct Point {
  /// Monday 00:00 is 0.
  uint16_t minute_of_week{0};
  /// The index of its mode in ScheduleConfig::modes.
  uint8_t mode{0};
};

/// The stored document of one schedule, without the JSON: the modes' actions in one list,
/// what they drive and the preset keys they name each kept once, the week one sorted list.
/// Its strings and lists are PSRAM first, as they grow with the document.
struct ScheduleConfig {
  /// SCHEDULE_VERSION, or the higher one of a file a newer firmware wrote.
  uint16_t version{SCHEDULE_VERSION};
  /// Moves on at every change of the document, so a Save of a form read before one is refused
  /// rather than undo it. 0 in a new one and in a file without it.
  uint32_t revision{0};
  PsramString id;
  PsramString name;
  bool enabled{true};
  PsramVector<Mode> modes;
  /// Every mode's actions, mode by mode.
  PsramVector<Action> actions;
  /// What the actions drive, each once, in the order the modes first name them.
  PsramVector<Target> targets;
  /// The preset keys the actions name, each once.
  PsramVector<PsramString> preset_keys;
  /// Sorted by minute_of_week.
  PsramVector<Point> points;

  /// Key order is fixed and all seven days are written, so a golden test can compare byte for byte.
  void serialize(JsonObject root) const;
  /// `{"type":"relay","id":"relay_1","value":{"type":"on"}}` and the like: an action as the file
  /// holds it.
  void write_action(JsonObject obj, const Action &action) const;
  /// The file's content in `doc`, refused when the next boot could not load it back: over
  /// `max_bytes`, or cut short by an allocation that failed.
  EncodeError encode(JsonDocument &doc, size_t max_bytes = MAX_FILE_BYTES) const;
  /// The same as text, `out` untouched on a refusal. The text is one block of the file's size:
  /// the engine writes `doc` out a buffer at a time instead.
  EncodeError encode(std::string *out, size_t max_bytes = MAX_FILE_BYTES,
                     ArduinoJson::Allocator *allocator = ArduinoJson::detail::DefaultAllocator::instance()) const;

  /// A broken rule is refused with a sentence in `error`, the first one in the editor's order:
  /// the name, the modes one at a time and each one's actions, then the week a day at a time.
  /// Names are trimmed, a missing day or list of actions reads as none, each day's points are
  /// sorted. An older version reads as this one; a newer one is kept, so the engine knows not to
  /// write the file.
  bool deserialize(JsonObjectConst root, bool require_id, std::string *error);
  /// The rules deserialize() applies, for a document built in C++: trim_names() and
  /// sort_points() first.
  bool validate(std::string *error) const;

  /// The schedule's and the modes' names without surrounding whitespace, as a file reads back.
  void trim_names();

  void sort_points();
  /// Builders for C++: a mode, then its actions, which go to the mode added last.
  void add_mode(std::string_view id, std::string_view name, uint8_t color = 0);
  void add_relay(std::string_view relay, bool on);
  void add_thermostat(std::string_view thermostat, ValueType value, std::string_view preset = "", float target = NAN);
  void add_wait(uint32_t seconds);
  /// Appends a point after any at the same minute, which validate() refuses.
  void add_point(uint16_t minute_of_week, uint8_t mode);

  const Action &action(const Mode &mode, size_t index) const { return this->actions[mode.first + index]; }
  /// A PRESET action's key, "" for any other.
  const PsramString &preset_of(const Action &action) const;
  /// What a RELAY or THERMOSTAT action drives.
  const Target &target_of(const Action &action) const { return this->targets[action.target]; }
  /// The index of the mode with this id, -1 for none.
  int mode_index(std::string_view id) const;
  bool names_target(const Target &target) const;

  /// The point whose mode holds at `minute_of_week`: the last one at or before it, or, before
  /// the week's first, the week's last. nullptr for no points.
  const Point *in_force(uint16_t minute_of_week) const;
  /// The first point after `minute_of_week`, going round into next week. nullptr for no points.
  const Point *next_after(uint16_t minute_of_week) const;

  /// About what the schedule holds in PSRAM, or on the heap without it, for the room checks.
  size_t heap_bytes() const;

  /// Written by a newer firmware: this one must not write it back.
  bool from_newer_firmware() const { return this->version > SCHEDULE_VERSION; }

 protected:
  Action &append_(ActionType type);
  uint8_t target_index_(TargetType type, std::string_view id);
  uint8_t preset_index_(std::string_view key);
};

/// Reduces a name to [a-z0-9-], collapsed and trimmed, at most ID_MAX_LENGTH characters;
/// `fallback` when nothing survives, as of a Cyrillic name.
std::string slugify_id(std::string_view name, const char *fallback = "schedule");
/// `base` with "-<n>" appended, cut so the whole stays within ID_MAX_LENGTH; n < 2 is `base`.
std::string id_with_suffix(const std::string &base, unsigned n);
/// The rules an id read from outside meets: present, and a slug.
bool validate_id(std::string_view id, std::string *error);

/// The name with surrounding ASCII whitespace removed.
std::string trim_name(std::string_view name);
/// The name rules on a trimmed name: 1..NAME_MAX_LENGTH characters of valid UTF-8, no control
/// characters, neither '/' nor '\'.
bool validate_name(std::string_view name, std::string *error);
/// The name as a person reads it: inner whitespace collapsed, ASCII lowercased.
std::string name_key(std::string_view name);
/// At most `chars` characters of valid UTF-8 `text`, never half of one.
std::string cut_chars(std::string_view text, size_t chars);

}  // namespace esphome::schedules
