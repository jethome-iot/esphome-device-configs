#pragma once

#include <cstddef>
#include <string>
#include "esphome/components/json/json_util.h"

namespace esphome::entity_config {

// In code points: what a panel row and the dashboard have room for.
inline constexpr size_t LABEL_MAX_LENGTH = 24;

// `text` trimmed of the spaces at both ends, into `out`. False, leaving `out` alone, for text
// that is not well-formed UTF-8, holds a control character or is over LABEL_MAX_LENGTH code
// points once trimmed. Empty is valid: no label.
bool parse_label(const char *text, size_t length, std::string &out);
// The same for a JSON value, which also has to be a string.
bool parse_label(JsonVariantConst value, std::string &out);

// The `label` field of the settings meta, the same for every type that has one.
void write_label_meta(JsonObject obj);

}  // namespace esphome::entity_config
