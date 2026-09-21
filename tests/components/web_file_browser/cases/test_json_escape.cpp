#include <gtest/gtest.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "esphome/components/web_file_browser/json_escape.h"

namespace esphome::web_file_browser::testing {

// U+FFFD, what the escaper answers for bytes that are not valid UTF-8.
static const std::string REPLACEMENT = "\xef\xbf\xbd";

// json_escape_ascii writes straight into the caller's buffer — the one place the escaper does
// — so a guard past the bound the streaming writer sizes its buffer on catches a write beyond
// the length the call reports.
static std::string escape_ascii(char c) {
  static const char GUARD = '\xAA';
  char buf[JSON_ESCAPE_MAX + 4];
  std::memset(buf, GUARD, sizeof(buf));
  const size_t len = json_escape_ascii(buf, c);
  const int byte = static_cast<uint8_t>(c);
  EXPECT_LE(len, JSON_ESCAPE_MAX) << byte;
  for (size_t i = std::min(len, sizeof(buf)); i < sizeof(buf); i++) {
    EXPECT_EQ(buf[i], GUARD) << "byte " << byte << " wrote " << len;
  }
  return std::string(buf, std::min(len, sizeof(buf)));
}

// The whole input through the escaper, end-of-input call included, checking on the way that no
// single step is longer than the bound the streaming writer keeps room for.
static std::string escape_all(const std::string &input) {
  std::string out;
  auto emit = [&out](const char *data, size_t size) {
    EXPECT_LE(size, JSON_ESCAPE_MAX) << "step of " << size << " bytes";
    out.append(data, size);
    return true;
  };
  JsonEscapeState state;
  for (char c : input) {
    json_escape_byte(state, c, emit);
  }
  json_escape_end(state, emit);
  return out;
}

static std::string escape_one(char c) { return escape_all(std::string(1, c)); }

static std::string all_bytes() {
  std::string input;
  for (int byte = 0; byte <= 0xff; byte++) {
    input += static_cast<char>(byte);
  }
  return input;
}

// A UTF-8 decoder of the test's own, so the answer is checked against the encoding rules
// rather than against the escaper that produced it.
static bool valid_utf8(const std::string &text) {
  for (size_t i = 0; i < text.size();) {
    const uint8_t lead = text[i];
    size_t len = 0;
    uint32_t code = 0;
    if (lead < 0x80) {
      len = 1;
      code = lead;
    } else if (lead >= 0xc0 && lead <= 0xdf) {
      len = 2;
      code = lead & 0x1fu;
    } else if (lead >= 0xe0 && lead <= 0xef) {
      len = 3;
      code = lead & 0x0fu;
    } else if (lead >= 0xf0 && lead <= 0xf7) {
      len = 4;
      code = lead & 0x07u;
    } else {
      return false;
    }
    if (i + len > text.size()) {
      return false;
    }
    for (size_t j = 1; j < len; j++) {
      const uint8_t next = text[i + j];
      if (next < 0x80 || next > 0xbf) {
        return false;
      }
      code = (code << 6) | (next & 0x3fu);
    }
    const uint32_t shortest[] = {0, 0, 0x80, 0x800, 0x10000};
    if (code < shortest[len] || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) {
      return false;
    }
    i += len;
  }
  return true;
}

// The RFC 8259 §7 string grammar over what goes between the quotes, plus the UTF-8 §8.1 asks
// of the whole text: together, what a byte-strict parser demands of /read's body.
static bool json_string_body(const std::string &body) {
  if (!valid_utf8(body)) {
    return false;
  }
  for (size_t i = 0; i < body.size(); i++) {
    const uint8_t byte = body[i];
    if (byte == '"' || byte < 0x20) {
      return false;
    }
    if (byte != '\\') {
      continue;
    }
    if (++i == body.size()) {
      return false;
    }
    const char escape = body[i];
    if (escape != '\0' && std::strchr("\"\\/bfnrt", escape) != nullptr) {
      continue;
    }
    if (escape != 'u' || i + 4 >= body.size()) {
      return false;
    }
    for (size_t j = 1; j <= 4; j++) {
      if (std::isxdigit(static_cast<unsigned char>(body[i + j])) == 0) {
        return false;
      }
    }
    i += 4;
  }
  return true;
}

TEST(JsonEscapeAscii, WritesTheNamedEscapes) {
  EXPECT_EQ(escape_ascii('"'), "\\\"");
  EXPECT_EQ(escape_ascii('\\'), "\\\\");
  EXPECT_EQ(escape_ascii('\b'), "\\b");
  EXPECT_EQ(escape_ascii('\f'), "\\f");
  EXPECT_EQ(escape_ascii('\n'), "\\n");
  EXPECT_EQ(escape_ascii('\r'), "\\r");
  EXPECT_EQ(escape_ascii('\t'), "\\t");
}

TEST(JsonEscapeAscii, SpellsOutTheRemainingControlBytes) {
  EXPECT_EQ(escape_ascii('\0'), "\\u0000");
  EXPECT_EQ(escape_ascii('\x01'), "\\u0001");
  EXPECT_EQ(escape_ascii('\x0b'), "\\u000b");
  EXPECT_EQ(escape_ascii('\x1f'), "\\u001f");
}

TEST(JsonEscapeAscii, PassesPrintableAsciiThrough) {
  for (char c : {' ', '!', '/', '0', 'A', 'z', '~', '\x7f'}) {
    EXPECT_EQ(escape_ascii(c), std::string(1, c)) << static_cast<int>(c);
  }
}

TEST(JsonEscapeByte, PassesValidUtf8Through) {
  // Both ends of every sequence length, the two sides of the surrogate hole, and characters a
  // file really carries: valid UTF-8 comes out byte for byte.
  for (const char *text : {"\xc2\x80", "\xc3\xa9", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xee\x80\x80",
                           "\xe2\x82\xac", "\xef\xbf\xbf", "\xf0\x90\x80\x80", "\xf0\x9f\x98\x80", "\xf4\x8f\xbf\xbf",
                           "\xef\xbf\xbd", "привет", "日本語"}) {
    EXPECT_EQ(escape_all(text), std::string(text)) << text;
  }
}

TEST(JsonEscapeByte, ReplacesEachIllFormedSequenceWithOneReplacement) {
  const std::vector<std::pair<std::string, const char *>> cases = {
      {"\x80", "a continuation byte with no lead"},
      {"\xbf", "a continuation byte with no lead"},
      {"\xf8", "a byte that starts nothing"},
      {"\xff", "a byte that starts nothing"},
      {"\xc3", "a two-byte sequence cut off by the end of the input"},
      {"\xe2\x82", "a three-byte sequence cut off by the end of the input"},
      {"\xf0\x9f\x98", "a four-byte sequence cut off by the end of the input"},
      {"\xc0\xaf", "an overlong two-byte /"},
      {"\xe0\x80\xaf", "an overlong three-byte /"},
      {"\xf0\x80\x80\xaf", "an overlong four-byte /"},
      {"\xc1\xbf", "an overlong two-byte DEL"},
      {"\xed\xa0\x80", "the high half of a surrogate pair"},
      {"\xed\xbf\xbf", "the low half of a surrogate pair"},
      {"\xf4\x90\x80\x80", "U+110000, one past the last code point"},
      {"\xf7\xbf\xbf\xbf", "U+1FFFFF, far past it"},
  };
  for (const auto &[input, what] : cases) {
    EXPECT_EQ(escape_all(input), REPLACEMENT) << what;
  }
}

TEST(JsonEscapeByte, StartsOverOnTheByteThatCutASequenceShort) {
  // The unfinished sequence is replaced where it stops; the byte that ended it is still the
  // input's, and is escaped in its own right.
  EXPECT_EQ(escape_all("\xe2\x82"
                       "a"),
            REPLACEMENT + "a");
  EXPECT_EQ(escape_all("\xe2\x82\x22"), REPLACEMENT + "\\\"");
  EXPECT_EQ(escape_all("\xf0\x9f\x01"), REPLACEMENT + "\\u0001");
  EXPECT_EQ(escape_all("\xc3\xe2\x82\xac"), REPLACEMENT + "\xe2\x82\xac");
  EXPECT_EQ(escape_all("\xe2\x82\xf8"), REPLACEMENT + REPLACEMENT);
}

TEST(JsonEscapeByte, LeavesValidUtf8AroundWhatItReplaces) {
  EXPECT_EQ(escape_all("caf\xc3\xa9\x80.txt"), "caf\xc3\xa9" + REPLACEMENT + ".txt");
  EXPECT_TRUE(valid_utf8(escape_all("caf\xc3\xa9\x80.txt")));
}

TEST(JsonEscapeByte, NeverWritesMoreThanTheBound) {
  // escape_all() checks every step against the bound, and escape_ascii() the guard past it,
  // for every byte there is.
  for (int byte = 0; byte <= 0xff; byte++) {
    escape_one(static_cast<char>(byte));
    if (byte < 0x80) {
      escape_ascii(static_cast<char>(byte));
    }
  }
}

// The streaming /read writer's shape: a fixed buffer, a sink taking what is flushed, the UTF-8
// state a chunk boundary can cut, and the tail the caller flushes itself once the input runs
// out. The buffer is exactly out_size bytes off the heap, so a write past it is an ASan report
// rather than a silent pass.
class Stream {
 public:
  explicit Stream(size_t out_size) : out_size_(out_size), buf_(new char[out_size]) {}

  // Feeds @p data in @p chunk-sized calls, as the file reader hands it over.
  bool write(const std::string &data, size_t chunk) {
    for (size_t i = 0; i < data.size(); i += chunk) {
      const size_t len = std::min(chunk, data.size() - i);
      if (!json_escape_chunk(data.data() + i, len, this->buf_.get(), this->out_size_, this->used_, this->state_,
                             [this](const char *out, size_t size) { return this->flush_(out, size); })) {
        return false;
      }
    }
    return true;
  }

  // The end-of-input call, which can flush a last time itself.
  bool end() {
    return json_escape_chunk_end(this->buf_.get(), this->out_size_, this->used_, this->state_,
                                 [this](const char *out, size_t size) { return this->flush_(out, size); });
  }

  // Everything the sink was handed, plus the unflushed tail — without the end-of-input call,
  // so a sequence still waiting for its continuation bytes is not in it yet.
  std::string so_far() const { return this->out_ + std::string(this->buf_.get(), this->used_); }

  // What the handler sends: the end-of-input call first, then the tail.
  std::string finish() {
    this->end();
    return this->so_far();
  }

  void refuse_from(size_t flush) { this->refuse_from_ = flush; }
  size_t used() const { return this->used_; }
  const std::vector<size_t> &flushes() const { return this->flushes_; }

 private:
  bool flush_(const char *out, size_t size) {
    this->flushes_.push_back(size);
    // Never more than the buffer holds, and never flushed while a whole further step would
    // still have fit. Neither says the flush came early enough — that is the buffer's own
    // size, which LeavesRoomForTheWidestStepAtEveryFill walks into.
    EXPECT_LE(size, this->out_size_);
    EXPECT_GT(size + JSON_ESCAPE_MAX, this->out_size_);
    if (this->refuse_from_ != 0 && this->flushes_.size() >= this->refuse_from_) {
      return false;
    }
    this->out_.append(out, size);
    return true;
  }

  size_t out_size_;
  std::unique_ptr<char[]> buf_;
  size_t used_{0};
  JsonEscapeState state_;
  size_t refuse_from_{0};
  std::string out_;
  std::vector<size_t> flushes_;
};

TEST(JsonEscapeChunk, EscapesWhatJsonForbids) {
  Stream stream(2 * JSON_ESCAPE_MAX);
  EXPECT_TRUE(stream.write("a\"b\\c\nd\te\x01", 3));
  // Not a raw string: a \u escape is read even inside one.
  EXPECT_EQ(stream.finish(), "a\\\"b\\\\c\\nd\\te\\u0001");
}

TEST(JsonEscapeChunk, CarriesTheTailBetweenCalls) {
  const std::string input = all_bytes();
  const std::string want = escape_all(input);
  // The chunk size is the file reader's, not the escaper's: the same bytes have to come out
  // however they arrive, and the smallest legal buffer is the one that flushes most.
  for (size_t chunk : {size_t{1}, size_t{7}, size_t{64}, input.size()}) {
    Stream stream(2 * JSON_ESCAPE_MAX);
    ASSERT_TRUE(stream.write(input, chunk)) << chunk;
    EXPECT_EQ(stream.finish(), want) << chunk;
  }
}

TEST(JsonEscapeChunk, CarriesASequenceAcrossAChunkBoundary) {
  // The reader's chunk size has nothing to do with UTF-8: a sequence arrives in two halves at
  // whatever offset, and still has to come out whole.
  for (const char *sequence : {"\xc3\xa9", "\xe2\x82\xac", "\xf0\x9f\x98\x80"}) {
    const std::string input = std::string("a") + sequence + "z";
    for (size_t split = 1; split < input.size(); split++) {
      Stream stream(2 * JSON_ESCAPE_MAX);
      ASSERT_TRUE(stream.write(input.substr(0, split), input.size())) << sequence << " at " << split;
      ASSERT_TRUE(stream.write(input.substr(split), input.size())) << sequence << " at " << split;
      EXPECT_EQ(stream.finish(), input) << sequence << " at " << split;
    }
  }
}

TEST(JsonEscapeChunk, ReplacesASequenceTheInputEndsInsideOf) {
  Stream stream(2 * JSON_ESCAPE_MAX);
  ASSERT_TRUE(stream.write("ok\xe2\x82", 8));
  // Held back: two bytes of three do not say what the character is yet.
  EXPECT_EQ(stream.so_far(), "ok");
  EXPECT_EQ(stream.finish(), "ok" + REPLACEMENT);
}

TEST(JsonEscapeChunk, AnswersAJsonStringWhateverTheBytes) {
  // What /read sends is `{"success":true,"content":"` + this + `"}`. A parser that decodes
  // strictly wants the whole text to be UTF-8 (RFC 8259 §8.1) and every control byte escaped
  // (§7); the bytes on the filesystem owe neither.
  const std::string input = all_bytes() + "\xed\xa0\x80\xf4\x90\x80\x80\xc0\xaf\xe2\x82\x22\xf0\x9f\x98\x80\xc3";
  ASSERT_FALSE(valid_utf8(input)) << "the checker would pass anything";
  ASSERT_FALSE(json_string_body(input)) << "the checker would pass anything";

  for (size_t chunk : {size_t{1}, size_t{5}, size_t{4096}}) {
    Stream stream(2 * JSON_ESCAPE_MAX);
    ASSERT_TRUE(stream.write(input, chunk)) << chunk;
    const std::string body = stream.finish();
    EXPECT_TRUE(valid_utf8(body)) << chunk;
    EXPECT_TRUE(json_string_body(body)) << chunk;
  }
  // The handlers themselves are behind USE_ESP32 and answer "Not supported on this platform"
  // on the host, so this is as far as the body can be followed here.
}

TEST(JsonEscapeChunk, KeepsAShortInputInTheBuffer) {
  Stream stream(64);
  EXPECT_TRUE(stream.write("ok", 1));
  EXPECT_TRUE(stream.flushes().empty());
  EXPECT_EQ(stream.used(), 2u);
  EXPECT_EQ(stream.finish(), "ok");
}

TEST(JsonEscapeChunk, LeavesRoomForTheWidestStepAtEveryFill) {
  // The writer's one job: flush early enough that the next step still fits. Nothing else here
  // can catch a flush that comes too late — the sink only ever sees what was flushed — so this
  // walks the fill to every value it reaches and then takes the widest step the escaper has,
  // against a buffer of exactly out_size heap bytes, where one byte too many is an ASan report.
  for (size_t out_size = JSON_ESCAPE_MAX; out_size <= 3 * JSON_ESCAPE_MAX; out_size++) {
    for (size_t fill = 0; fill <= out_size; fill++) {
      // 'a' is a one-byte step, so `fill` of them leave every reachable fill behind at some
      // point; \x01 is , the six bytes of JSON_ESCAPE_MAX.
      const std::string filler(fill, 'a');
      const std::string where = std::to_string(out_size) + " bytes, fill " + std::to_string(fill);
      {
        Stream stream(out_size);
        ASSERT_TRUE(stream.write(filler + "\x01", filler.size() + 1)) << where;
        EXPECT_EQ(stream.finish(), filler + "\\u0001") << where;
      }
      {
        // The same step, reached from one input byte that emits twice: the replacement for the
        // sequence it cut short and then its own escape, with no room check in between but the
        // writer's own.
        Stream stream(out_size);
        ASSERT_TRUE(stream.write(filler + "\xe2\x82\x01", filler.size() + 3)) << where;
        EXPECT_EQ(stream.finish(), filler + REPLACEMENT + "\\u0001") << where;
      }
    }
  }
}

TEST(JsonEscapeChunk, StopsWhenTheSinkRefuses) {
  // A sink says false when the response is gone; the walk must not go on escaping into a
  // buffer nobody drains.
  Stream stream(2 * JSON_ESCAPE_MAX);
  stream.refuse_from(1);
  EXPECT_FALSE(stream.write(all_bytes(), 16));
  EXPECT_EQ(stream.flushes().size(), 1u);
}

}  // namespace esphome::web_file_browser::testing
