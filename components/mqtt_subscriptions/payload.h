#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include "esphome/core/optional.h"
#include "slot_config.h"

namespace esphome::mqtt_subscriptions {

// Larger messages are not read: the client has already reassembled them, but nothing here
// should hold more.
static constexpr size_t PAYLOAD_MAX = 2048;
// What a slot keeps of its last message, for the dashboard.
static constexpr size_t RAW_MAX = 64;
static constexpr size_t TEXT_MAX = 255;

// One message as every slot on its topic reads it: the JSON is parsed once, by the first slot
// that has a path.
class Message {
 public:
  explicit Message(const std::string &payload) : payload_(payload) {}

  const std::string &payload() const { return this->payload_; }
  // The parsed payload, nullptr when it is not JSON.
  const JsonDocument *json();

 protected:
  const std::string &payload_;
  std::unique_ptr<Document> document_;
  bool parsed_{false};
};

// NaN is unknown: an empty payload, a null-like value, or one that failed to read.
struct NumberReading {
  float value;
  std::string error;
};
// No value is unknown.
struct BinaryReading {
  optional<bool> value;
  std::string error;
};
// No value leaves the text sensor as it was.
struct TextReading {
  optional<std::string> value;
  std::string error;
};

NumberReading read_number(Message &message, const std::string &json_path);
BinaryReading read_binary(Message &message, const std::string &json_path, const std::string &payload_on,
                          const std::string &payload_off);
TextReading read_text(Message &message, const std::string &json_path);

// The first RAW_MAX bytes, cut on a character, with stray bytes and control characters as '?'
// (tabs and line breaks as spaces).
std::string raw_preview(const std::string &payload);

// Well-formed UTF-8: no overlong forms, surrogates, code points past U+10FFFF or cut sequences.
bool utf8_valid(const std::string &text);
// The length of the longest prefix of at most `max_bytes` that ends on a character boundary.
size_t utf8_cut(const std::string &text, size_t max_bytes);

}  // namespace esphome::mqtt_subscriptions
