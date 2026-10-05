#pragma once

#include <ArduinoJson.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "enums.h"
#include "esphome/components/climate/climate_mode.h"
#include "esphome/core/entity_base.h"
#include "esphome/core/optional.h"

namespace esphome::climate_hub {

/// A document larger than this is refused before it is parsed.
static constexpr size_t CONFIG_MAX_BYTES = 8192;
/// The file format this firmware writes. A file with a higher one came from a newer firmware:
/// it is read as far as this one understands it and never written back.
static constexpr uint16_t CONFIG_VERSION = 2;
/// Presets per thermostat.
static constexpr size_t PRESET_MAX_COUNT = 8;
/// Longest name a controller may have; its entity keeps it in a fixed buffer.
static constexpr size_t NAME_MAX_LENGTH = 48;
/// Longest id; `<id>.json.tmp` has to stay inside LittleFS's 64-byte names.
static constexpr size_t ID_MAX_LENGTH = 48;
/// Longest sensor_id or relay_id: an object id has one character per byte of the entity's
/// name, and upstream caps a name at this many bytes.
static constexpr size_t ENTITY_ID_MAX_LENGTH = ESPHOME_FRIENDLY_NAME_MAX_LEN;

/// Why ClimateConfig::encode() refused.
enum class EncodeError : uint8_t { NONE, TOO_LARGE, NO_MEMORY };

/// One driven direction. An empty relay_id means the direction is unused.
struct OutputConfig {
  std::string relay_id;
  float period_s{300.f};
  float min_on_s{10.f};
  float min_off_s{10.f};

  bool configured() const { return !this->relay_id.empty(); }
};

struct VisualConfig {
  float min_temperature{5.f};
  float max_temperature{45.f};
  float step{0.5f};
};

struct SafetyConfig {
  float sensor_timeout_s{300.f};
  float max_temperature{60.f};
};

struct PidParams {
  float kp{0.6f};
  float ki{0.0025f};
  float kd{0.f};
  float min_integral{-1.f};
  float max_integral{1.f};
  float starting_integral_term{0.f};
  float output_samples{1.f};
  float derivative_samples{8.f};
  float deadband_threshold_low{0.f};
  float deadband_threshold_high{0.f};
  float deadband_kp_multiplier{0.f};
  float deadband_ki_multiplier{0.f};
  float deadband_kd_multiplier{0.f};
  float deadband_output_samples{1.f};
};

/// The band around the target, as deviations rather than absolute points: the controller
/// switches at `setpoint - below` and `setpoint + above`.
struct BangBangParams {
  float below{0.5f};
  float above{0.5f};
};

/// A target, and a mode or none, that a thermostat takes in one step.
struct PresetConfig {
  /// The slug of the name it was created with; a rename keeps it. The file, the API and the
  /// rules name a preset by it.
  std::string key;
  std::string name;
  float setpoint{NAN};
  /// None keeps the thermostat's mode.
  optional<HubMode> mode;
};

/// The stored document of one controller. Keyed on `id`, never on the name, so a rename
/// re-keys nothing: not the file, not the relay claims.
struct ClimateConfig {
  /// CONFIG_VERSION, or the higher one of a file a newer firmware wrote.
  uint16_t version{CONFIG_VERSION};
  std::string id;
  std::string name;
  bool enabled{true};
  // A relay-driven boiler or floor loop is what these boards switch, and it needs no tuning.
  ControlKind kind{ControlKind::BANG_BANG};
  std::string sensor_id;
  float update_interval_s{30.f};
  OutputConfig heat;
  OutputConfig cool;
  VisualConfig visual;
  SafetyConfig safety;
  PidParams pid;
  BangBangParams bang_bang;
  HubMode mode{HubMode::HEAT};

  // One target for both algorithms; bang-bang derives its two switching points from it, so the
  // entity never needs climate::Climate's two-point union.
  float setpoint{21.f};

  /// In the order Home Assistant lists the custom ones.
  std::vector<PresetConfig> presets;
  /// The key of the preset picked last, "" for none. A target or a mode set by hand keeps it.
  std::string active_preset;

  /// Key order is fixed so a golden test can compare byte for byte.
  void serialize(JsonObject root) const;

  /// The file's content. Refused, `out` untouched, when the next boot could not load it back:
  /// over `max_bytes`, or cut short by an allocation that failed.
  EncodeError encode(std::string *out, size_t max_bytes = CONFIG_MAX_BYTES,
                     ArduinoJson::Allocator *allocator = ArduinoJson::detail::DefaultAllocator::instance()) const;

  /// Numbers are clamped, a broken rule is refused with a sentence in `error`: the first one
  /// in the editor's order, the presets after the thermostat's own rules, its name rules last.
  /// The names are trimmed, the setpoints clamped into the visual range, a preset without a key
  /// gets one, and an active_preset no preset has is dropped. An older version reads as this
  /// one; a newer one is kept, so the hub knows not to write the file.
  bool deserialize(const JsonObject &root, bool require_id, std::string *error);

  /// The rules deserialize() applies, in its order, for a document built in C++.
  bool validate(std::string *error) const;

  /// Clamps every tunable number into its range in the param table, NaN to its default, as
  /// deserialize() does.
  void clamp_numbers();

  /// Holds the setpoint and every preset's inside the visual range.
  void clamp_setpoint();

  /// Gives every preset without a key the slug of its name, "-2" and on when that is taken.
  void assign_preset_keys();
  /// The preset with this key, nullptr for none.
  const PresetConfig *find_preset(const std::string &key) const;
  /// The preset whose name is Home Assistant's built-in `preset`, or that is the custom `name`.
  const PresetConfig *find_preset(climate::ClimatePreset preset) const;
  const PresetConfig *find_custom_preset(const char *name) const;
  /// Takes the preset's target, clamped, and its mode if it has one the relays serve, and
  /// labels it active. True when anything changed.
  bool pick_preset(const PresetConfig &preset);

  /// Written by a newer firmware: this one must not write it back.
  bool from_newer_firmware() const { return this->version > CONFIG_VERSION; }
  /// The bang-bang switching points. Never stored: the band is.
  float switch_low() const { return this->setpoint - this->bang_bang.below; }
  float switch_high() const { return this->setpoint + this->bang_bang.above; }

  bool supports_heat() const { return this->heat.configured(); }
  bool supports_cool() const { return this->cool.configured(); }
  bool supports_mode(HubMode mode) const;
  /// `value` held inside the visual range.
  float clamp_target(float value) const;
};

/// Reduces a display name to [a-z0-9-], collapsed and trimmed, at most ID_MAX_LENGTH chars.
/// `fallback` when nothing survives.
std::string slugify_id(const std::string &name, const char *fallback = "climate");

/// `base` with "-<n>" appended, cut so the whole stays within ID_MAX_LENGTH; n < 2 is `base`.
std::string id_with_suffix(const std::string &base, unsigned n);

/// The name with surrounding ASCII whitespace removed.
std::string trim_name(const std::string &name);

/// The name rules on a trimmed name: 1..NAME_MAX_LENGTH printable ASCII characters, neither
/// '/' (a web_server URL cannot carry it) nor '\' (a browser turns it into '/').
bool validate_name(const std::string &name, std::string *error);

/// The name as a person reads it: trimmed, inner whitespace collapsed, ASCII lowercased.
std::string name_key(const std::string &name);

/// Whether `name`, in any case, is one of Home Assistant's built-in presets (eco, away, boost,
/// comfort, home, sleep, activity), and which. Upstream maps such a string to the built-in one
/// before it looks at the custom presets, so a preset by that name has to be the built-in.
bool standard_preset(const std::string &name, climate::ClimatePreset *out = nullptr);

/// The object id ESPHome derives from an entity name: what Home Assistant and every
/// object-id-keyed record in this repository know the entity by.
std::string object_id_of_name(const std::string &name);

}  // namespace esphome::climate_hub
