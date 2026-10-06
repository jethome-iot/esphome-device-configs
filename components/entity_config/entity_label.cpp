#include "entity_label.h"
#include <cstdint>

namespace esphome::entity_config {

namespace {

// The length of the well-formed UTF-8 sequence at `p`, 0 when there is none. The ranges are
// Unicode's table of well-formed sequences: no overlong form, no surrogate, nothing past U+10FFFF.
size_t sequence_at(const uint8_t *p, const uint8_t *end, uint32_t &code_point) {
  const uint8_t lead = *p;
  if (lead < 0x80) {
    code_point = lead;
    return 1;
  }
  size_t length;
  uint8_t low = 0x80;
  uint8_t high = 0xBF;
  if (lead < 0xC2) {
    return 0;
  } else if (lead < 0xE0) {
    length = 2;
  } else if (lead < 0xF0) {
    length = 3;
    if (lead == 0xE0)
      low = 0xA0;
    if (lead == 0xED)
      high = 0x9F;
  } else if (lead < 0xF5) {
    length = 4;
    if (lead == 0xF0)
      low = 0x90;
    if (lead == 0xF4)
      high = 0x8F;
  } else {
    return 0;
  }
  if (static_cast<size_t>(end - p) < length)
    return 0;
  code_point = lead & (0x7F >> length);
  for (size_t i = 1; i < length; i++) {
    if (p[i] < (i == 1 ? low : 0x80) || p[i] > (i == 1 ? high : 0xBF))
      return 0;
    code_point = (code_point << 6) | (p[i] & 0x3F);
  }
  return length;
}

bool is_control(uint32_t code_point) { return code_point < 0x20 || (code_point >= 0x7F && code_point <= 0x9F); }

}  // namespace

bool parse_label(const char *text, size_t length, std::string &out) {
  if (text == nullptr)
    return false;
  const auto *begin = reinterpret_cast<const uint8_t *>(text);
  const auto *end = begin + length;
  while (begin < end && *begin == ' ')
    begin++;
  while (end > begin && end[-1] == ' ')
    end--;
  size_t code_points = 0;
  for (const uint8_t *p = begin; p < end;) {
    uint32_t code_point = 0;
    const size_t taken = sequence_at(p, end, code_point);
    if (taken == 0 || is_control(code_point) || ++code_points > LABEL_MAX_LENGTH)
      return false;
    p += taken;
  }
  out.assign(reinterpret_cast<const char *>(begin), end - begin);
  return true;
}

bool parse_label(JsonVariantConst value, std::string &out) {
  if (!value.is<const char *>())
    return false;
  // With its size: a NUL decoded from "\u0000" is a control character, not the end.
  const JsonString text = value.as<JsonString>();
  return parse_label(text.c_str(), text.size(), out);
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
