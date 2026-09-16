#pragma once

#include <cstdio>
#include <cstdint>
#include <cstddef>

namespace esphome {
namespace web_file_browser {

/// Longest escape a single byte can produce: \u00XX.
static const size_t JSON_ESCAPE_MAX = 6;

/// JSON-escapes one byte into @p out and returns how many bytes it wrote, never
/// more than JSON_ESCAPE_MAX. The streaming /read writer sizes its buffer on that
/// bound, so keep it true.
inline size_t json_escape_char(char *out, char c) {
  switch (c) {
    case '"':
      out[0] = '\\';
      out[1] = '"';
      return 2;
    case '\\':
      out[0] = '\\';
      out[1] = '\\';
      return 2;
    case '\b':
      out[0] = '\\';
      out[1] = 'b';
      return 2;
    case '\f':
      out[0] = '\\';
      out[1] = 'f';
      return 2;
    case '\n':
      out[0] = '\\';
      out[1] = 'n';
      return 2;
    case '\r':
      out[0] = '\\';
      out[1] = 'r';
      return 2;
    case '\t':
      out[0] = '\\';
      out[1] = 't';
      return 2;
    default:
      break;
  }
  // The cast keeps the bound at six bytes: plain char is unsigned on xtensa, but
  // a signed one would send every byte above 0x7f down this branch as a negative.
  if (static_cast<uint8_t>(c) < 0x20) {
    char buf[JSON_ESCAPE_MAX + 1];
    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<uint8_t>(c));
    for (size_t i = 0; i < JSON_ESCAPE_MAX; i++) {
      out[i] = buf[i];
    }
    return JSON_ESCAPE_MAX;
  }
  out[0] = c;
  return 1;
}

/// Escapes @p len bytes of @p data into @p out, flushing it through @p sink —
/// sink(data, size), false to stop — whenever fewer than JSON_ESCAPE_MAX bytes
/// are left, which keeps the next byte's worst-case escape inside the buffer
/// without ever growing it. @p used carries the unflushed tail between calls and
/// must be flushed by the caller once the input runs out. Requires
/// out_size >= 2 * JSON_ESCAPE_MAX and used + JSON_ESCAPE_MAX <= out_size.
template<typename Sink>
bool json_escape_chunk(const char *data, size_t len, char *out, size_t out_size, size_t &used, Sink sink) {
  for (size_t i = 0; i < len; i++) {
    used += json_escape_char(out + used, data[i]);
    if (used + JSON_ESCAPE_MAX > out_size) {
      if (!sink(out, used)) {
        return false;
      }
      used = 0;
    }
  }
  return true;
}

}  // namespace web_file_browser
}  // namespace esphome
