#pragma once

#include <cstddef>
#include <string>
#include "esphome/components/json/json_util.h"
#include "esphome/components/panel_text/panel_text.h"

namespace esphome::entity_config {

// The rules are panel_text's, shared with the temperature slots' labels.
using panel_text::LABEL_MAX_LENGTH;
using panel_text::parse_label;

// The same for a JSON value, which also has to be a string.
bool parse_label(JsonVariantConst value, std::string &out);

// The `label` field of the settings meta, the same for every type that has one.
void write_label_meta(JsonObject obj);

}  // namespace esphome::entity_config
