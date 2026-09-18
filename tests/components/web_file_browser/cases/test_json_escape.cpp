#include <gtest/gtest.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/web_file_browser/json_escape.h"

namespace esphome::web_file_browser::testing {

// One byte through json_escape_char, with a guard past the bound the streaming writer sizes
// its buffer on: anything written beyond what the call reports would overflow that buffer.
static std::string escape_one(char c) {
  static const char GUARD = '\xAA';
  char buf[JSON_ESCAPE_MAX + 4];
  std::memset(buf, GUARD, sizeof(buf));
  const size_t len = json_escape_char(buf, c);
  const int byte = static_cast<uint8_t>(c);
  EXPECT_LE(len, JSON_ESCAPE_MAX) << byte;
  for (size_t i = std::min(len, sizeof(buf)); i < sizeof(buf); i++) {
    EXPECT_EQ(buf[i], GUARD) << "byte " << byte << " wrote " << len;
  }
  return std::string(buf, std::min(len, sizeof(buf)));
}

TEST(JsonEscapeChar, WritesTheNamedEscapes) {
  EXPECT_EQ(escape_one('"'), "\\\"");
  EXPECT_EQ(escape_one('\\'), "\\\\");
  EXPECT_EQ(escape_one('\b'), "\\b");
  EXPECT_EQ(escape_one('\f'), "\\f");
  EXPECT_EQ(escape_one('\n'), "\\n");
  EXPECT_EQ(escape_one('\r'), "\\r");
  EXPECT_EQ(escape_one('\t'), "\\t");
}

TEST(JsonEscapeChar, SpellsOutTheRemainingControlBytes) {
  EXPECT_EQ(escape_one('\0'), "\\u0000");
  EXPECT_EQ(escape_one('\x01'), "\\u0001");
  EXPECT_EQ(escape_one('\x0b'), "\\u000b");
  EXPECT_EQ(escape_one('\x1f'), "\\u001f");
}

TEST(JsonEscapeChar, PassesPrintableAsciiThrough) {
  for (char c : {' ', '!', '/', '0', 'A', 'z', '~', '\x7f'}) {
    EXPECT_EQ(escape_one(c), std::string(1, c)) << static_cast<int>(c);
  }
}

TEST(JsonEscapeChar, PassesHighBytesThroughOneAtATime) {
  // Plain char is signed on the host: read as a number, each of these is negative and would
  // fall into the control-byte branch, cutting every UTF-8 name into six bytes per byte.
  for (int byte = 0x80; byte <= 0xff; byte++) {
    const char c = static_cast<char>(byte);
    EXPECT_EQ(escape_one(c), std::string(1, c)) << byte;
  }
}

TEST(JsonEscapeChar, NeverWritesMoreThanTheBound) {
  // escape_one() checks the reported length and the guard for every byte there is.
  for (int byte = 0; byte <= 0xff; byte++) {
    escape_one(static_cast<char>(byte));
  }
}

// The streaming /read writer's shape: a fixed buffer, a sink taking what is flushed, and the
// tail the caller flushes itself once the input runs out. The buffer is exactly out_size bytes
// off the heap, so a write past it is an ASan report rather than a silent pass.
class Stream {
 public:
  explicit Stream(size_t out_size) : out_size_(out_size), buf_(new char[out_size]) {}

  // Feeds @p data in @p chunk-sized calls, as the file reader hands it over.
  bool write(const std::string &data, size_t chunk) {
    for (size_t i = 0; i < data.size(); i += chunk) {
      const size_t len = std::min(chunk, data.size() - i);
      if (!json_escape_chunk(data.data() + i, len, this->buf_.get(), this->out_size_, this->used_,
                             [this](const char *out, size_t size) { return this->flush_(out, size); })) {
        return false;
      }
    }
    return true;
  }

  // Everything the sink was handed, plus the unflushed tail.
  std::string finish() const { return this->out_ + std::string(this->buf_.get(), this->used_); }

  void refuse_from(size_t flush) { this->refuse_from_ = flush; }
  size_t used() const { return this->used_; }
  const std::vector<size_t> &flushes() const { return this->flushes_; }

 private:
  bool flush_(const char *out, size_t size) {
    this->flushes_.push_back(size);
    // Flushed only once the next byte's worst case would no longer fit, and never more than
    // the buffer holds.
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
  size_t refuse_from_{0};
  std::string out_;
  std::vector<size_t> flushes_;
};

// json_escape_char over the whole input: what the chunked walk has to add up to.
static std::string escape_all(const std::string &input) {
  std::string out;
  char buf[JSON_ESCAPE_MAX];
  for (char c : input) {
    out.append(buf, json_escape_char(buf, c));
  }
  return out;
}

static std::string all_bytes() {
  std::string input;
  for (int byte = 0; byte <= 0xff; byte++) {
    input += static_cast<char>(byte);
  }
  return input;
}

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

TEST(JsonEscapeChunk, KeepsAShortInputInTheBuffer) {
  Stream stream(64);
  EXPECT_TRUE(stream.write("ok", 1));
  EXPECT_TRUE(stream.flushes().empty());
  EXPECT_EQ(stream.used(), 2u);
  EXPECT_EQ(stream.finish(), "ok");
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
