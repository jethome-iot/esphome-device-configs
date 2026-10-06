#include "common.h"

namespace esphome::entity_config::testing {

// What parse_label() makes of `text`, or "<refused>".
static std::string label_of(const std::string &text) {
  std::string out = "<untouched>";
  if (!parse_label(text.data(), text.size(), out))
    return out == "<untouched>" ? "<refused>" : "<refused, but wrote '" + out + "'>";
  return out;
}

static std::string label_of_json(const char *json) {
  JsonDocument doc = body(json);
  std::string out = "<untouched>";
  if (!parse_label(doc["label"], out))
    return out == "<untouched>" ? "<refused>" : "<refused, but wrote '" + out + "'>";
  return out;
}

TEST(Label, TextIsKeptAsItIs) {
  EXPECT_EQ(label_of("Kitchen light"), "Kitchen light");
  EXPECT_EQ(label_of("Свет на кухне"), "Свет на кухне");
  EXPECT_EQ(label_of("Ёлка: 21 °C"), "Ёлка: 21 °C");
  EXPECT_EQ(label_of("a  b"), "a  b");  // only the ends are trimmed
}

TEST(Label, EmptyIsNoLabel) {
  EXPECT_EQ(label_of(""), "");
  EXPECT_EQ(label_of("   "), "");
}

TEST(Label, SpacesAtBothEndsAreTrimmed) {
  EXPECT_EQ(label_of("  Kitchen light "), "Kitchen light");
  EXPECT_EQ(label_of(" Свет "), "Свет");
  // A no-break space is a character of the label, not a space to trim.
  EXPECT_EQ(label_of("\xC2\xA0x\xC2\xA0"), "\xC2\xA0x\xC2\xA0");
}

TEST(Label, TheLengthIsInCodePointsNotBytes) {
  const std::string latin(24, 'x');
  EXPECT_EQ(label_of(latin), latin);
  EXPECT_EQ(label_of(latin + "x"), "<refused>");

  std::string cyrillic;
  for (int i = 0; i < 24; i++)
    cyrillic += "ж";  // 48 bytes
  EXPECT_EQ(label_of(cyrillic), cyrillic);
  EXPECT_EQ(label_of(cyrillic + "ж"), "<refused>");

  std::string emoji;
  for (int i = 0; i < 24; i++)
    emoji += "\xF0\x9F\x92\xA1";  // 96 bytes
  EXPECT_EQ(label_of(emoji), emoji);
  EXPECT_EQ(label_of(emoji + "\xF0\x9F\x92\xA1"), "<refused>");
}

TEST(Label, TheLengthIsCountedAfterTrimming) {
  const std::string full(24, 'x');
  EXPECT_EQ(label_of("   " + full + "   "), full);
}

TEST(Label, AControlCharacterIsRefused) {
  for (const std::string text : {"a\nb", "a\tb", "Kitchen\r", "\x1B[1m", "a\x7F", "a\xC2\x85", "\xC2\x9F"}) {
    EXPECT_EQ(label_of(text), "<refused>") << ::testing::PrintToString(text);
  }
  EXPECT_EQ(label_of(std::string("a\0b", 3)), "<refused>");
}

TEST(Label, MalformedUtf8IsRefused) {
  for (const std::string text : {
           "\x80",              // a continuation byte with no lead
           "\xFF",              // a byte no sequence starts with
           "\xC0\x80",          // an overlong NUL
           "\xC1\x81",          // an overlong 'A'
           "\xE0\x80\xAF",      // an overlong '/'
           "\xED\xA0\x80",      // a surrogate, U+D800
           "\xF4\x90\x80\x80",  // past U+10FFFF
           "\xF5\x80\x80\x80",  // a lead past the last plane
           "ab\xE2\x82",        // cut short at the end
           "\xE2\x82"
           "b",         // cut short before more text
           "Свет\xD0",  // half a Cyrillic letter
       }) {
    EXPECT_EQ(label_of(text), "<refused>") << ::testing::PrintToString(text);
  }
}

TEST(Label, TheEdgesOfEveryRangeAreWellFormed) {
  for (const std::string text : {"\xC2\xA0", "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80",
                                 "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF"}) {
    EXPECT_EQ(label_of(text), text) << ::testing::PrintToString(text);
  }
}

TEST(Label, NullIsRefused) {
  std::string out = "kept";
  EXPECT_FALSE(parse_label(nullptr, 0, out));
  EXPECT_EQ(out, "kept");
}

TEST(Label, AJsonValueHasToBeAString) {
  EXPECT_EQ(label_of_json(R"({"label":" Kitchen "})"), "Kitchen");
  EXPECT_EQ(label_of_json(R"({"label":""})"), "");
  for (const char *json : {R"({"label":5})", R"({"label":true})", R"({"label":["a"]})", R"({"label":{"a":1}})",
                           R"({"label":null})", R"({})"}) {
    EXPECT_EQ(label_of_json(json), "<refused>") << json;
  }
}

// What JSON escapes decode to goes through the same rules as raw text.
TEST(Label, AnEscapedControlCharacterIsRefused) {
  EXPECT_EQ(label_of_json(R"({"label":"a\u0000b"})"), "<refused>");
  EXPECT_EQ(label_of_json(R"({"label":"a\nb"})"), "<refused>");
  EXPECT_EQ(label_of_json(R"({"label":"\u0007"})"), "<refused>");
  EXPECT_EQ(label_of_json(R"({"label":"Ж"})"), "Ж");
}

TEST(Label, TheMetaFieldIsAStringOfAtMost24) {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  write_label_meta(obj);
  std::string text;
  serializeJson(doc, text);
  EXPECT_EQ(text, R"({"label":{"type":"string","label":"Label","description":"Shown on the panel and the dashboard )"
                  R"(in place of the name. Empty shows the name.","default":"","max_length":24}})");
}

}  // namespace esphome::entity_config::testing
