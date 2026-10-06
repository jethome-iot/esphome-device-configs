#include <gtest/gtest.h>
#include <string>
#include "esphome/components/panel_text/panel_text.h"

namespace esphome::panel_text::testing {

// What parse_label() makes of `text`, or "<refused>".
static std::string label_of(const std::string &text) {
  std::string out = "<untouched>";
  if (!parse_label(text.data(), text.size(), out))
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
  EXPECT_EQ(LABEL_MAX_LENGTH, 24u);
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
  EXPECT_EQ(label_of(std::string("\0", 1)), "<refused>");
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

// Only the given length is read: a NUL inside is a character, the bytes after the end are not.
TEST(Label, OnlyTheGivenLengthIsRead) {
  std::string out;
  EXPECT_TRUE(parse_label("Kitchen\nlight", 7, out));
  EXPECT_EQ(out, "Kitchen");
}

// A label passes panel_safe() whole with a font that has its glyphs: the rules are the same.
TEST(Label, ALabelIsTextThePanelDrawsWhole) {
  const auto every_glyph = [](uint32_t) { return true; };
  for (const std::string text : {"Kitchen light", "Подача", "Ёлка: 21 °C", "\xF0\x9F\x92\xA1"}) {
    std::string out;
    ASSERT_TRUE(parse_label(text.data(), text.size(), out)) << text;
    EXPECT_EQ(panel_safe(out, 4 * LABEL_MAX_LENGTH, every_glyph), out) << text;
  }
}

}  // namespace esphome::panel_text::testing
