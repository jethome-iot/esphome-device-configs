#pragma once

#include <ArduinoJson.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include "enums.h"

namespace esphome::climate_hub {

/// A document larger than this is refused before it is parsed.
static constexpr size_t CONFIG_MAX_BYTES = 8192;
/// Longest name a controller may have; its entity keeps it in a fixed buffer.
static constexpr size_t NAME_MAX_LENGTH = 48;
/// Longest id; `<id>.json.tmp` has to stay inside LittleFS's 64-byte names.
static constexpr size_t ID_MAX_LENGTH = 48;

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

/// The stored document of one controller. Keyed on `id`, never on the name, so a rename
/// re-keys nothing: not the file, not the relay claims.
struct ClimateConfig {
  uint16_t version{1};
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

  /// Key order is fixed so a golden test can compare byte for byte.
  void serialize(JsonObject root) const;

  /// Numbers are clamped, structure is refused with a sentence in `error`. The name is
  /// trimmed, and the setpoint clamped into the visual range.
  bool deserialize(const JsonObject &root, bool require_id, std::string *error);

  /// The structural rules deserialize() ends with, for a document built in C++.
  bool validate(std::string *error) const;

  /// Holds the setpoint inside the visual range.
  void clamp_setpoint();

  /// The bang-bang switching points. Never stored: the band is.
  float switch_low() const { return this->setpoint - this->bang_bang.below; }
  float switch_high() const { return this->setpoint + this->bang_bang.above; }

  bool supports_heat() const { return this->heat.configured(); }
  bool supports_cool() const { return this->cool.configured(); }
};

/// Reduces a display name to [a-z0-9-], collapsed and trimmed, at most ID_MAX_LENGTH chars.
/// "climate" when nothing survives.
std::string slugify_id(const std::string &name);

/// The name with surrounding ASCII whitespace removed.
std::string trim_name(const std::string &name);

/// The name rules on a trimmed name: 1..NAME_MAX_LENGTH printable ASCII characters, neither
/// '/' (a web_server URL cannot carry it) nor '\' (a browser turns it into '/').
bool validate_name(const std::string &name, std::string *error);

/// The name as a person reads it: trimmed, inner whitespace collapsed, ASCII lowercased.
std::string name_key(const std::string &name);

/// The object id ESPHome derives from an entity name: what Home Assistant and every
/// object-id-keyed record in this repository know the entity by.
std::string object_id_of_name(const std::string &name);

}  // namespace esphome::climate_hub
