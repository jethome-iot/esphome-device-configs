#include "common.h"
#include <cmath>

namespace esphome::mqtt_subscriptions::testing {

static NumberReading number(const std::string &payload, const std::string &path = "") {
  Message message(payload);
  return read_number(message, path);
}

static BinaryReading binary(const std::string &payload, const std::string &path = "", const std::string &on = "ON",
                            const std::string &off = "OFF") {
  Message message(payload);
  return read_binary(message, path, on, off);
}

static TextReading text(const std::string &payload, const std::string &path = "") {
  Message message(payload);
  return read_text(message, path);
}

#define EXPECT_NUMBER(reading, expected) \
  do { \
    const NumberReading r_ = (reading); \
    EXPECT_FLOAT_EQ(r_.value, (expected)); \
    EXPECT_EQ(r_.error, ""); \
  } while (0)

#define EXPECT_UNKNOWN(reading, expected_error) \
  do { \
    const NumberReading r_ = (reading); \
    EXPECT_TRUE(std::isnan(r_.value)); \
    EXPECT_EQ(r_.error, (expected_error)); \
  } while (0)

TEST(NumberPayload, PlainText) {
  EXPECT_NUMBER(number("21.5"), 21.5f);
  EXPECT_NUMBER(number("  -3 \r\n"), -3.0f);
  EXPECT_NUMBER(number("1e3"), 1000.0f);
  EXPECT_UNKNOWN(number("21.5 °C"), "not a number");
  EXPECT_UNKNOWN(number("warm"), "not a number");
  EXPECT_UNKNOWN(number("true"), "not a number");
}

TEST(NumberPayload, NullLikeIsUnknownWithoutAnError) {
  for (const char *word : {"null", "NaN", "None", "unknown", "UNAVAILABLE", "   "}) {
    SCOPED_TRACE(word);
    EXPECT_UNKNOWN(number(word), "");
  }
  // A cleared retained message.
  EXPECT_UNKNOWN(number(""), "");
  EXPECT_UNKNOWN(number("", "temperature"), "");
}

TEST(NumberPayload, NotFiniteIsUnknown) {
  EXPECT_UNKNOWN(number("inf"), "");
  EXPECT_UNKNOWN(number("-Infinity"), "");
  EXPECT_UNKNOWN(number("1e99"), "");
  EXPECT_UNKNOWN(number(R"({"t":1e99})", "t"), "");
}

TEST(NumberPayload, JsonValues) {
  const std::string z2m = R"({"battery":97,"temperature":21.4,"humidity":48,"ok":true,"text":" 7.5 ",)"
                          R"("none":null,"obj":{"a":1},"list":[1,2],"word":"warm","gone":"unavailable"})";
  EXPECT_NUMBER(number(z2m, "temperature"), 21.4f);
  EXPECT_NUMBER(number(z2m, "battery"), 97.0f);
  EXPECT_NUMBER(number(z2m, "ok"), 1.0f);
  EXPECT_NUMBER(number(R"({"ok":false})", "ok"), 0.0f);
  EXPECT_NUMBER(number(z2m, "text"), 7.5f);
  EXPECT_UNKNOWN(number(z2m, "none"), "");
  EXPECT_UNKNOWN(number(z2m, "gone"), "");
  EXPECT_UNKNOWN(number(z2m, "obj"), "not a number");
  EXPECT_UNKNOWN(number(z2m, "list"), "not a number");
  EXPECT_UNKNOWN(number(z2m, "word"), "not a number");
  // A bare JSON number is a document too, with no key in it.
  EXPECT_UNKNOWN(number("42", "x"), "key 'x' not found");
}

TEST(NumberPayload, PathsWalkObjectsAndArrays) {
  const std::string doc = R"({"a":{"b":[{"c":1},{"c":2}],"0":5},"list":[[10,11],[20,21]]})";
  EXPECT_NUMBER(number(doc, "a.b.1.c"), 2.0f);
  EXPECT_NUMBER(number(doc, "list.1.0"), 20.0f);
  // A digit key indexes an object as well, by name.
  EXPECT_NUMBER(number(doc, "a.0"), 5.0f);
  EXPECT_UNKNOWN(number(doc, "a.b.2.c"), "key 'a.b.2.c' not found");
  EXPECT_UNKNOWN(number(doc, "a.b.x"), "key 'a.b.x' not found");
  EXPECT_UNKNOWN(number(doc, "a.b.1.c.d"), "key 'a.b.1.c.d' not found");
  EXPECT_UNKNOWN(number(doc, "list.9999999999"), "key 'list.9999999999' not found");
  EXPECT_UNKNOWN(number(doc, "missing"), "key 'missing' not found");
  EXPECT_UNKNOWN(number("21.5 °C", "temperature"), "not JSON");
}

TEST(NumberPayload, OverTwoKibIsNotRead) {
  const std::string large = R"({"t":1,"pad":")" + std::string(PAYLOAD_MAX, 'x') + R"("})";
  EXPECT_UNKNOWN(number(large, "t"), "message over 2 KiB");
  const std::string just = std::string(PAYLOAD_MAX - 4, ' ') + "21.5";
  EXPECT_NUMBER(number(just), 21.5f);
}

TEST(BinaryPayload, ThePayloadsWithoutCase) {
  EXPECT_EQ(binary("ON").value, optional<bool>(true));
  EXPECT_EQ(binary(" off ").value, optional<bool>(false));
  EXPECT_EQ(binary("open", "", "Open", "Closed").value, optional<bool>(true));
  EXPECT_EQ(binary("CLOSED", "", "Open", "Closed").value, optional<bool>(false));
  const BinaryReading neither = binary("maybe");
  EXPECT_FALSE(neither.value.has_value());
  EXPECT_EQ(neither.error, "neither ON nor OFF");
}

TEST(BinaryPayload, AJsonBoolIsTheFallback) {
  EXPECT_EQ(binary("true").value, optional<bool>(true));
  EXPECT_EQ(binary("false").value, optional<bool>(false));
  EXPECT_EQ(binary(R"({"contact":true})", "contact").value, optional<bool>(true));
  EXPECT_EQ(binary(R"({"contact":false})", "contact").value, optional<bool>(false));
  // Text is not a bool: "TRUE" matches neither payload.
  EXPECT_FALSE(binary("TRUE").value.has_value());
}

// zigbee2mqtt's contact is true while the door is shut: the payloads turn it round.
TEST(BinaryPayload, ThePayloadsComeBeforeTheBool) {
  EXPECT_EQ(binary(R"({"contact":true})", "contact", "false", "true").value, optional<bool>(false));
  EXPECT_EQ(binary(R"({"contact":false})", "contact", "false", "true").value, optional<bool>(true));
  EXPECT_EQ(binary("1", "", "1", "0").value, optional<bool>(true));
  EXPECT_EQ(binary(R"({"state":1})", "state", "1", "0").value, optional<bool>(true));
  EXPECT_EQ(binary(R"({"state":"ON"})", "state").value, optional<bool>(true));
}

TEST(BinaryPayload, UnknownWithAndWithoutAnError) {
  EXPECT_EQ(binary("").error, "");
  EXPECT_FALSE(binary("").value.has_value());
  EXPECT_EQ(binary(R"({"contact":null})", "contact").error, "");
  EXPECT_FALSE(binary(R"({"contact":null})", "contact").value.has_value());
  EXPECT_EQ(binary(R"({"contact":{}})", "contact").error, "neither ON nor OFF");
  EXPECT_EQ(binary("ON", "contact").error, "not JSON");
  EXPECT_EQ(binary(R"({"a":1})", "contact").error, "key 'contact' not found");
  EXPECT_EQ(binary(std::string(PAYLOAD_MAX + 1, 'x')).error, "message over 2 KiB");
}

TEST(TextPayload, AsSentOrSerialized) {
  EXPECT_EQ(text("  hello world \n").value, optional<std::string>("hello world"));
  EXPECT_EQ(text(R"({"a":{"b":[1,"x"]}})", "a").value, optional<std::string>(R"({"b":[1,"x"]})"));
  EXPECT_EQ(text(R"({"a":"Включён"})", "a").value, optional<std::string>("Включён"));
  EXPECT_EQ(text(R"({"a":12.5})", "a").value, optional<std::string>("12.5"));
  EXPECT_EQ(text(R"({"a":null})", "a").value, optional<std::string>("null"));
  EXPECT_EQ(text("a\nb").value, optional<std::string>("a\nb"));
  // A cleared retained message empties it.
  EXPECT_EQ(text("").value, optional<std::string>(""));
}

TEST(TextPayload, CutAt255BytesOnACharacter) {
  const std::string ascii(300, 'a');
  EXPECT_EQ(text(ascii).value->size(), TEXT_MAX);
  std::string cyrillic;
  for (int i = 0; i < 200; i++)
    cyrillic += "ж";  // two bytes each: 255 would cut the 128th in half
  const std::string cut = *text(cyrillic).value;
  EXPECT_EQ(cut.size(), 254u);
  EXPECT_TRUE(utf8_valid(cut));
}

TEST(TextPayload, InvalidUtf8LeavesTheStateAlone) {
  const TextReading reading = text("ab\xFF");
  EXPECT_FALSE(reading.value.has_value());
  EXPECT_EQ(reading.error, "not UTF-8");
  EXPECT_EQ(text("x", "a").error, "not JSON");
  EXPECT_EQ(text(R"({"b":1})", "a").error, "key 'a' not found");
  EXPECT_FALSE(text(std::string(PAYLOAD_MAX + 1, 'x')).value.has_value());
}

TEST(Utf8, WellFormedOnly) {
  EXPECT_TRUE(utf8_valid(""));
  EXPECT_TRUE(utf8_valid("aä€😀"));
  for (const char *bad :
       {"\x80", "\xC1\x81", "\xC3", "\xE0\x80\x80", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\xF5\x80", "\xE2\x82"}) {
    SCOPED_TRACE(bad);
    EXPECT_FALSE(utf8_valid(bad));
  }
  EXPECT_EQ(utf8_cut("abc", 5), 3u);
  EXPECT_EQ(utf8_cut("aä", 2), 1u);
  EXPECT_EQ(utf8_cut("a€", 3), 1u);
  EXPECT_EQ(utf8_cut("a€", 4), 4u);
}

TEST(RawPreview, FirstBytesSanitized) {
  EXPECT_EQ(raw_preview(R"({"t":1})"), R"({"t":1})");
  EXPECT_EQ(raw_preview("a\tb\nc\rd"), "a b c d");
  EXPECT_EQ(raw_preview(std::string("a\0b\x1b", 4)), "a?b?");
  EXPECT_EQ(raw_preview("a\xFF\xC3z"), "a??z");
  EXPECT_EQ(raw_preview(std::string(100, 'x')).size(), RAW_MAX);
  std::string cyrillic;
  for (int i = 0; i < 40; i++)
    cyrillic += "ж";
  const std::string cut = raw_preview(cyrillic);
  EXPECT_EQ(cut.size(), RAW_MAX);
  EXPECT_TRUE(utf8_valid(cut));
  std::string odd = "a";
  for (int i = 0; i < 40; i++)
    odd += "ж";
  EXPECT_EQ(raw_preview(odd).size(), RAW_MAX - 1);
}

// A message whose slots all read it whole is never parsed, nor one over the limit.
class PeekMessage : public Message {
 public:
  using Message::Message;
  bool parsed() const { return this->parsed_; }
};

TEST(MessageParse, NotForASlotWithoutAPath) {
  const std::string payload = R"({"a":1})";
  PeekMessage message(payload);
  read_number(message, "");
  read_binary(message, "", "ON", "OFF");
  read_text(message, "");
  EXPECT_FALSE(message.parsed());
  EXPECT_FLOAT_EQ(read_number(message, "a").value, 1.0f);
  EXPECT_TRUE(message.parsed());

  const std::string large = R"({"a":")" + std::string(PAYLOAD_MAX, 'x') + R"("})";
  PeekMessage too_large(large);
  read_number(too_large, "a");
  EXPECT_FALSE(too_large.parsed());
}

// The JSON is parsed once, whatever number of slots read the message.
TEST(MessageParse, OnceAndOnlyWhenAsked) {
  const std::string payload = R"({"a":1})";
  Message message(payload);
  const JsonDocument *first = message.json();
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(message.json(), first);
  const std::string bad = "{";
  Message broken(bad);
  EXPECT_EQ(broken.json(), nullptr);
  EXPECT_EQ(broken.json(), nullptr);
}

}  // namespace esphome::mqtt_subscriptions::testing
