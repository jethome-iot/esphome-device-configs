#pragma once

#include <cstdio>
#include <cstdint>
#include <cstddef>

namespace esphome {
namespace web_file_browser {

/// Longest run of bytes one escape step can produce: \u00XX.
static const size_t JSON_ESCAPE_MAX = 6;

/// U+FFFD, what every byte sequence that is not valid UTF-8 is replaced with: a
/// JSON text is UTF-8 (RFC 8259 §8.1) and the bytes on the filesystem need not be.
/// One per ill-formed sequence on purpose, not one per maximal subpart the way
/// Unicode §3.9 and a browser's TextDecoder do it: F5 80 80 80 is one replacement
/// here and four there. All the body owes a client is that the bytes were not text.
static const char JSON_ESCAPE_REPLACEMENT[] = "\xef\xbf\xbd";
static const size_t JSON_ESCAPE_REPLACEMENT_LEN = sizeof(JSON_ESCAPE_REPLACEMENT) - 1;

/// The start of a UTF-8 sequence, carried to the call that gets the rest of it.
struct JsonEscapeState {
  uint8_t seq[4]{};   ///< the bytes seen so far
  uint8_t len{0};     ///< how many of them there are
  uint8_t expect{0};  ///< how many the lead byte announced; 0 between sequences
};

/// JSON-escapes one ASCII byte into @p out and returns how many bytes it wrote,
/// never more than JSON_ESCAPE_MAX. Only for bytes json_escape_byte() has already
/// found to be ASCII: anything above 0x7f belongs to a UTF-8 sequence, which the
/// state machine there has to see whole.
inline size_t json_escape_ascii(char *out, char c) {
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
  // Cast because plain char is unsigned on xtensa and signed on the host; the
  // comparison has to mean the same thing on both.
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

/// Whether the completed sequence in @p state is the one encoding of a real code
/// point: the shortest form, not half a surrogate pair, not past U+10FFFF.
inline bool json_escape_sequence_valid(const JsonEscapeState &state) {
  uint32_t code = state.seq[0] & (0xffu >> (state.expect + 1));
  for (uint8_t i = 1; i < state.expect; i++) {
    code = (code << 6) | (state.seq[i] & 0x3fu);
  }
  const uint32_t shortest[] = {0, 0, 0x80, 0x800, 0x10000};
  return code >= shortest[state.expect] && code <= 0x10ffff && (code < 0xd800 || code > 0xdfff);
}

/// Feeds one byte to the escaper, handing what comes out to @p emit —
/// emit(data, size), false to stop — in steps of at most JSON_ESCAPE_MAX bytes
/// each. A byte inside a UTF-8 sequence emits nothing and waits in @p state for the
/// rest of it, so one byte can also emit twice: the replacement for a sequence that
/// was never finished, then the escape of the byte that cut it short.
template<typename Emit> bool json_escape_byte(JsonEscapeState &state, char c, Emit emit) {
  const uint8_t byte = static_cast<uint8_t>(c);

  if (state.expect != 0) {
    if (byte >= 0x80 && byte <= 0xbf) {
      state.seq[state.len++] = byte;
      if (state.len < state.expect) {
        return true;
      }
      const bool valid = json_escape_sequence_valid(state);
      const size_t len = state.expect;
      state.len = 0;
      state.expect = 0;
      if (!valid) {
        return emit(JSON_ESCAPE_REPLACEMENT, JSON_ESCAPE_REPLACEMENT_LEN);
      }
      return emit(reinterpret_cast<const char *>(state.seq), len);
    }
    // The sequence never got the continuation bytes it announced: it goes out as
    // one U+FFFD and this byte starts over below.
    state.len = 0;
    state.expect = 0;
    if (!emit(JSON_ESCAPE_REPLACEMENT, JSON_ESCAPE_REPLACEMENT_LEN)) {
      return false;
    }
  }

  if (byte < 0x80) {
    char out[JSON_ESCAPE_MAX];
    return emit(out, json_escape_ascii(out, c));
  }

  uint8_t expect = 0;
  if (byte >= 0xc0 && byte <= 0xdf) {
    expect = 2;
  } else if (byte >= 0xe0 && byte <= 0xef) {
    expect = 3;
  } else if (byte >= 0xf0 && byte <= 0xf7) {
    expect = 4;
  }
  // A continuation byte with nothing in front of it, or 0xf8..0xff, which starts
  // nothing at all.
  if (expect == 0) {
    return emit(JSON_ESCAPE_REPLACEMENT, JSON_ESCAPE_REPLACEMENT_LEN);
  }
  state.seq[0] = byte;
  state.len = 1;
  state.expect = expect;
  return true;
}

/// Ends the input: a sequence it stopped in the middle of becomes one U+FFFD.
/// Every caller owes the escaper this call, or a name or a file that ends inside a
/// sequence loses its last character.
template<typename Emit> bool json_escape_end(JsonEscapeState &state, Emit emit) {
  if (state.expect == 0) {
    return true;
  }
  state.len = 0;
  state.expect = 0;
  return emit(JSON_ESCAPE_REPLACEMENT, JSON_ESCAPE_REPLACEMENT_LEN);
}

/// The emit json_escape_chunk() hands the escaper: appends one step to @p out and
/// flushes it through @p sink — sink(data, size), false to stop — whenever fewer
/// than JSON_ESCAPE_MAX bytes are left, which keeps the next step inside the buffer
/// without ever growing it. The room is checked per step, not per input byte: one
/// byte can emit a replacement and an escape, nine bytes between them.
template<typename Sink> class JsonEscapeBuffer {
 public:
  JsonEscapeBuffer(char *out, size_t out_size, size_t &used, Sink sink)
      : out_(out), out_size_(out_size), used_(used), sink_(sink) {}

  bool operator()(const char *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
      this->out_[this->used_ + i] = data[i];
    }
    this->used_ += size;
    if (this->used_ + JSON_ESCAPE_MAX > this->out_size_) {
      if (!this->sink_(this->out_, this->used_)) {
        return false;
      }
      this->used_ = 0;
    }
    return true;
  }

 private:
  char *out_;
  size_t out_size_;
  size_t &used_;
  Sink sink_;
};

/// Escapes @p len bytes of @p data into @p out, flushed through @p sink as
/// JsonEscapeBuffer describes. @p used carries the unflushed tail between calls and
/// @p state the UTF-8 sequence a chunk boundary cut in half; once the input runs
/// out the caller calls json_escape_chunk_end() and then flushes @p used itself.
/// Requires out_size >= JSON_ESCAPE_MAX and used + JSON_ESCAPE_MAX <= out_size.
template<typename Sink>
bool json_escape_chunk(const char *data, size_t len, char *out, size_t out_size, size_t &used, JsonEscapeState &state,
                       Sink sink) {
  JsonEscapeBuffer<Sink> emit(out, out_size, used, sink);
  for (size_t i = 0; i < len; i++) {
    if (!json_escape_byte(state, data[i], emit)) {
      return false;
    }
  }
  return true;
}

/// json_escape_end() for the streaming caller: same buffer, same sink, and it can
/// flush too, so it belongs before the caller sends what @p used still holds.
template<typename Sink>
bool json_escape_chunk_end(char *out, size_t out_size, size_t &used, JsonEscapeState &state, Sink sink) {
  JsonEscapeBuffer<Sink> emit(out, out_size, used, sink);
  return json_escape_end(state, emit);
}

}  // namespace web_file_browser
}  // namespace esphome
