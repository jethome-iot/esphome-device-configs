#include "payload.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace esphome::mqtt_subscriptions {

static const char *const TOO_LARGE = "message over 2 KiB";

const JsonDocument *Message::json() {
  if (!this->parsed_) {
    this->parsed_ = true;
    auto document = std::make_unique<Document>();
    if (!deserializeJson(document->doc, this->payload_.data(), this->payload_.size()))
      this->document_ = std::move(document);
  }
  return this->document_ == nullptr ? nullptr : &this->document_->doc;
}

// The byte length of the well-formed sequence at `p`, 0 for a stray or cut one.
static size_t sequence_at(const uint8_t *p, const uint8_t *end, uint32_t &cp) {
  const uint8_t lead = *p;
  size_t extra;
  if (lead < 0x80) {
    cp = lead;
    return 1;
  } else if (lead >= 0xC2 && lead <= 0xDF) {
    cp = lead & 0x1F;
    extra = 1;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    cp = lead & 0x0F;
    extra = 2;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    cp = lead & 0x07;
    extra = 3;
  } else {
    return 0;
  }
  if (static_cast<size_t>(end - p) <= extra)
    return 0;
  for (size_t i = 1; i <= extra; i++) {
    if ((p[i] & 0xC0) != 0x80)
      return 0;
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
    return 0;
  return extra + 1;
}

bool utf8_valid(const std::string &text) {
  const auto *p = reinterpret_cast<const uint8_t *>(text.data());
  const auto *end = p + text.size();
  while (p < end) {
    uint32_t cp;
    const size_t len = sequence_at(p, end, cp);
    if (len == 0)
      return false;
    p += len;
  }
  return true;
}

size_t utf8_cut(const std::string &text, size_t max_bytes) {
  if (text.size() <= max_bytes)
    return text.size();
  size_t cut = max_bytes;
  while (cut > 0 && (static_cast<uint8_t>(text[cut]) & 0xC0) == 0x80)
    cut--;
  return cut;
}

std::string raw_preview(const std::string &payload) {
  std::string out;
  const auto *p = reinterpret_cast<const uint8_t *>(payload.data());
  const auto *end = p + payload.size();
  while (p < end) {
    uint32_t cp;
    const size_t len = sequence_at(p, end, cp);
    const char *piece;
    size_t piece_len = 1;
    if (len == 0) {
      piece = "?";
      p++;
    } else {
      if (cp == '\t' || cp == '\n' || cp == '\r') {
        piece = " ";
      } else if (cp < 0x20 || (cp >= 0x7F && cp <= 0x9F)) {
        piece = "?";
      } else {
        piece = reinterpret_cast<const char *>(p);
        piece_len = len;
      }
      p += len;
    }
    if (out.size() + piece_len > RAW_MAX)
      break;
    out.append(piece, piece_len);
  }
  return out;
}

namespace {

// Where a slot's value sits in a message: the JSON value at its path, or the payload's text.
struct Located {
  std::string error;
  bool json{false};
  JsonVariantConst value;
  std::string text;
};

bool all_digits(const std::string &key) {
  return !key.empty() && std::all_of(key.begin(), key.end(), [](char c) { return c >= '0' && c <= '9'; });
}

Located locate(Message &message, const std::string &json_path) {
  Located out;
  if (json_path.empty()) {
    out.text = trim_ascii(message.payload());
    return out;
  }
  const JsonDocument *document = message.json();
  if (document == nullptr) {
    out.error = "not JSON";
    return out;
  }
  JsonVariantConst node = document->as<JsonVariantConst>();
  size_t start = 0;
  while (true) {
    const size_t dot = json_path.find('.', start);
    const std::string key = json_path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
    if (node.is<JsonArrayConst>()) {
      // Nine digits always fit; a longer index is past any array a message can hold.
      node = all_digits(key) && key.size() <= 9
                 ? node.as<JsonArrayConst>()[static_cast<size_t>(std::strtoul(key.c_str(), nullptr, 10))]
                 : JsonVariantConst();
    } else if (node.is<JsonObjectConst>()) {
      node = node.as<JsonObjectConst>()[key];
    } else {
      node = JsonVariantConst();
    }
    if (node.isUnbound()) {
      out.error = "key '" + json_path + "' not found";
      return out;
    }
    if (dot == std::string::npos)
      break;
    start = dot + 1;
  }
  out.json = true;
  out.value = node;
  return out;
}

bool null_like(const std::string &text) {
  for (const char *word : {"", "nan", "none", "null", "unknown", "unavailable"}) {
    if (equal_ignoring_case(text, word))
      return true;
  }
  return false;
}

float finite_or_nan(float value) { return std::isfinite(value) ? value : NAN; }

NumberReading number_from_text(const std::string &raw) {
  const std::string text = trim_ascii(raw);
  if (null_like(text))
    return {NAN, ""};
  char *end = nullptr;
  const float value = std::strtof(text.c_str(), &end);
  if (end == text.c_str() || *end != '\0')
    return {NAN, "not a number"};
  return {finite_or_nan(value), ""};
}

}  // namespace

NumberReading read_number(Message &message, const std::string &json_path) {
  if (message.payload().size() > PAYLOAD_MAX)
    return {NAN, TOO_LARGE};
  if (message.payload().empty())
    return {NAN, ""};  // a cleared retained message
  const Located at = locate(message, json_path);
  if (!at.error.empty())
    return {NAN, at.error};
  if (!at.json)
    return number_from_text(at.text);
  const JsonVariantConst value = at.value;
  if (value.isNull())
    return {NAN, ""};
  if (value.is<bool>())
    return {value.as<bool>() ? 1.0f : 0.0f, ""};
  if (value.is<float>())
    return {finite_or_nan(value.as<float>()), ""};
  if (value.is<const char *>())
    return number_from_text(value.as<std::string>());
  return {NAN, "not a number"};
}

BinaryReading read_binary(Message &message, const std::string &json_path, const std::string &payload_on,
                          const std::string &payload_off) {
  if (message.payload().size() > PAYLOAD_MAX)
    return {{}, TOO_LARGE};
  if (message.payload().empty())
    return {{}, ""};
  const Located at = locate(message, json_path);
  if (!at.error.empty())
    return {{}, at.error};
  std::string text;
  optional<bool> literal;
  if (at.json) {
    if (at.value.isNull())
      return {{}, ""};
    if (at.value.is<const char *>()) {
      text = at.value.as<std::string>();
    } else {
      serializeJson(at.value, text);
      if (at.value.is<bool>())
        literal = at.value.as<bool>();
    }
  } else {
    text = at.text;
    // A payload of just `true` or `false` is a JSON bool as well.
    if (text == "true") {
      literal = true;
    } else if (text == "false") {
      literal = false;
    }
  }
  // The payloads first, so a slot can read a bool the other way round.
  if (equal_ignoring_case(text, payload_on))
    return {true, ""};
  if (equal_ignoring_case(text, payload_off))
    return {false, ""};
  if (literal.has_value())
    return {literal, ""};
  return {{}, "neither ON nor OFF"};
}

TextReading read_text(Message &message, const std::string &json_path) {
  if (message.payload().size() > PAYLOAD_MAX)
    return {{}, TOO_LARGE};
  if (message.payload().empty())
    return {std::string(), ""};
  const Located at = locate(message, json_path);
  if (!at.error.empty())
    return {{}, at.error};
  std::string text;
  if (!at.json) {
    text = at.text;
  } else if (at.value.is<const char *>()) {
    text = at.value.as<std::string>();
  } else {
    serializeJson(at.value, text);
  }
  if (!utf8_valid(text))
    return {{}, "not UTF-8"};
  text.resize(utf8_cut(text, TEXT_MAX));
  return {text, ""};
}

}  // namespace esphome::mqtt_subscriptions
