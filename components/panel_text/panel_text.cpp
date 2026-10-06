#include "panel_text.h"
#include <algorithm>

namespace esphome::panel_text {

static const char *const ELLIPSIS = "\xE2\x80\xA6";  // U+2026

namespace {

// A lead byte's sequence length and the range its first continuation byte must fall in, as
// Unicode's table of well-formed sequences has them: the ranges rule out overlong forms,
// surrogates and anything past U+10FFFF. Length 0 is a byte no sequence starts with.
struct Lead {
  uint8_t length;
  uint8_t low;
  uint8_t high;
};

Lead lead_of(uint8_t byte) {
  if (byte < 0x80)
    return {1, 0, 0};
  if (byte < 0xC2)
    return {0, 0, 0};
  if (byte < 0xE0)
    return {2, 0x80, 0xBF};
  if (byte == 0xE0)
    return {3, 0xA0, 0xBF};
  if (byte == 0xED)
    return {3, 0x80, 0x9F};
  if (byte < 0xF0)
    return {3, 0x80, 0xBF};
  if (byte == 0xF0)
    return {4, 0x90, 0xBF};
  if (byte < 0xF4)
    return {4, 0x80, 0xBF};
  if (byte == 0xF4)
    return {4, 0x80, 0x8F};
  return {0, 0, 0};
}

bool is_control(uint32_t code_point) { return code_point < 0x20 || (code_point >= 0x7F && code_point <= 0x9F); }

bool is_lead(char byte) { return (static_cast<uint8_t>(byte) & 0xC0) != 0x80; }

// The sequence at `p`: the bytes it takes, a well-formed sequence whole or else its maximal
// ill-formed subpart (one byte at least), and the length its lead byte announced.
struct Decoded {
  uint32_t code_point;
  size_t taken;
  uint8_t length;
  bool whole() const { return this->length != 0 && this->taken == this->length; }
};

Decoded decode_at(const uint8_t *p, const uint8_t *end) {
  const Lead lead = lead_of(*p);
  uint32_t code_point = lead.length == 1 ? *p : *p & (0x7F >> lead.length);
  size_t taken = 1;
  while (taken < lead.length && p + taken < end) {
    const uint8_t next = p[taken];
    if (next < (taken == 1 ? lead.low : 0x80) || next > (taken == 1 ? lead.high : 0xBF))
      break;
    code_point = (code_point << 6) | (next & 0x3F);
    taken++;
  }
  return {code_point, taken, lead.length};
}

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
    const Decoded decoded = decode_at(p, end);
    if (!decoded.whole() || is_control(decoded.code_point) || ++code_points > LABEL_MAX_LENGTH)
      return false;
    p += decoded.taken;
  }
  out.assign(reinterpret_cast<const char *>(begin), end - begin);
  return true;
}

std::string panel_safe(const std::string &text, size_t max_bytes, const HasGlyph &has_glyph) {
  const bool cut = text.size() > max_bytes;
  const size_t end = cut ? max_bytes : text.size();
  const auto *bytes = reinterpret_cast<const uint8_t *>(text.data());
  std::string out;
  out.reserve(end + 3);
  size_t i = 0;
  while (i < end) {
    const Decoded decoded = decode_at(bytes + i, bytes + end);
    // Split by the cut rather than broken: the '…' below stands for it.
    if (cut && decoded.taken < decoded.length && i + decoded.taken == end)
      break;
    if (decoded.whole() && !is_control(decoded.code_point) && has_glyph(decoded.code_point)) {
      out.append(text, i, decoded.taken);
    } else {
      out += '?';
    }
    i += decoded.taken;
  }
  if (cut)
    out += ELLIPSIS;
  return out;
}

size_t panel_glyphs(const std::string &text) {
  return static_cast<size_t>(std::count_if(text.begin(), text.end(), is_lead));
}

std::string panel_fit(const std::string &text, size_t glyphs) {
  if (panel_glyphs(text) <= glyphs)
    return text;
  if (glyphs == 0)
    return "";
  size_t kept = 0;
  size_t i = 0;
  for (; i < text.size(); i++) {
    if (is_lead(text[i]) && kept++ == glyphs - 1)
      break;
  }
  // "Outdoor…", not "Outdoor …".
  while (i > 0 && text[i - 1] == ' ')
    i--;
  return text.substr(0, i) + ELLIPSIS;
}

std::string panel_pair(const std::string &name, const std::string &value, size_t width) {
  const size_t name_glyphs = panel_glyphs(name);
  const size_t value_glyphs = panel_glyphs(value);
  if (name_glyphs + 2 + value_glyphs <= width)
    return name + ": " + value;
  if (width < 3)
    return panel_fit(name + ": " + value, width);
  const size_t room = width - 2;
  const size_t name_room = std::min(room, std::max<size_t>(6, room > value_glyphs ? room - value_glyphs : 0));
  const size_t keep = std::min(name_glyphs, name_room);
  return panel_fit(name, keep) + ": " + panel_fit(value, room - keep);
}

}  // namespace esphome::panel_text
