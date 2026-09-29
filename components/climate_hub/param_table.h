#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::climate_hub {

/// One tunable number: its limits, its default and how a form should render it.
struct ParamDesc {
  const char *key;
  const char *label;
  const char *unit;
  const char *group;
  /// The algorithm this knob belongs to, or "" when both use it. The form hides what the
  /// selected algorithm ignores; the codec does not care.
  const char *kind;
  float def;
  float min;
  float max;
  float step;
  bool integer;
  /// One sentence the form shows on hover: the device describes its own knobs.
  const char *hint;
};

/// The whole tunable surface. Served to the editor as its schema and used to clamp the stored
/// document, so the form and the firmware cannot disagree.
extern const ParamDesc PARAMS[];
extern const size_t PARAM_COUNT;

const ParamDesc *find_param(const char *key);

/// Clamps to the descriptor's range; the default for NaN, the value itself for an unknown key.
float clamp_param(const char *key, float value);

}  // namespace esphome::climate_hub
