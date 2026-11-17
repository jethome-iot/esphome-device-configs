#include "case_fold.h"
#include <algorithm>
#include <iterator>
#include "case_fold_table.h"

namespace esphome::case_fold {

namespace {

// The sequence of one to three bytes at `i` and its code point, or 0 when there is none there.
// Nothing at U+10000 or above folds, so a four-byte one passes byte by byte.
size_t decode(std::string_view text, size_t i, uint32_t *code_point) {
  const auto lead = static_cast<uint8_t>(text[i]);
  if (lead < 0x80) {
    *code_point = lead;
    return 1;
  }
  size_t length;
  // An overlong form is not UTF-8 and passes as it is. A surrogate decodes, but nothing there
  // folds.
  uint8_t low = 0x80;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    if (lead == 0xE0)
      low = 0xA0;
  } else {
    return 0;
  }
  if (text.size() - i < length)
    return 0;
  uint32_t value = lead & (0x7F >> length);
  for (size_t k = 1; k < length; k++) {
    const auto next = static_cast<uint8_t>(text[i + k]);
    if (next < (k == 1 ? low : 0x80) || next > 0xBF)
      return 0;
    value = (value << 6) | (next & 0x3F);
  }
  *code_point = value;
  return length;
}

// Two or three bytes: the table is 16-bit, and nothing but ASCII folds into ASCII.
void append_utf8(std::string &out, uint32_t code_point) {
  if (code_point < 0x800) {
    out += static_cast<char>(0xC0 | (code_point >> 6));
  } else {
    out += static_cast<char>(0xE0 | (code_point >> 12));
    out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
  }
  out += static_cast<char>(0x80 | (code_point & 0x3F));
}

}  // namespace

uint32_t fold_code_point(uint32_t code_point) {
  // Only the last range that starts at or before the code point can hold it.
  const auto *after = std::upper_bound(std::begin(FOLD_RANGES), std::end(FOLD_RANGES), code_point,
                                       [](uint32_t c, const FoldRange &range) { return c < range.first; });
  if (after == std::begin(FOLD_RANGES))
    return code_point;
  const FoldRange &range = *std::prev(after);
  if (code_point > range.last || (code_point - range.first) % range.stride != 0)
    return code_point;
  return static_cast<uint32_t>(static_cast<int32_t>(code_point) + range.delta);
}

std::string fold_case(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    uint32_t code_point = 0;
    const size_t length = decode(text, i, &code_point);
    if (length == 0) {
      out += text[i++];
      continue;
    }
    const uint32_t folded = fold_code_point(code_point);
    if (folded == code_point) {
      out.append(text.data() + i, length);
    } else if (folded < 0x80) {
      out += static_cast<char>(folded);
    } else {
      append_utf8(out, folded);
    }
    i += length;
  }
  return out;
}

}  // namespace esphome::case_fold
