#include "entity_label.h"

namespace esphome::entity_config {

bool parse_label(JsonVariantConst value, std::string &out) {
  if (!value.is<const char *>())
    return false;
  // With its size: a NUL decoded from "\u0000" is a control character, not the end.
  const JsonString text = value.as<JsonString>();
  return panel_text::parse_label(text.c_str(), text.size(), out);
}

void write_label_meta(JsonObject obj) {
  JsonObject field = obj["label"].to<JsonObject>();
  field["type"] = "string";
  field["label"] = "Label";
  field["description"] = "Shown on the panel and the dashboard in place of the name. Empty shows the name.";
  field["default"] = "";
  field["max_length"] = LABEL_MAX_LENGTH;
}

}  // namespace esphome::entity_config
