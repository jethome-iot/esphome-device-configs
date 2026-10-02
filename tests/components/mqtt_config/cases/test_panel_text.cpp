#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <vector>
#include "esphome/components/mqtt_config/panel_text.h"

namespace esphome::mqtt_config::testing {

// A stand-in for the menu font, not its glyph list: ASCII, Cyrillic and the symbols these cases use.
static bool font_stand_in(uint32_t code_point) {
  return (code_point >= 0x20 && code_point <= 0x7E) || (code_point >= 0x400 && code_point <= 0x4FF) ||
         code_point == 0xB0 || code_point == 0xB2 || code_point == 0xB3 || code_point == 0xB5 || code_point == 0x2026;
}
static bool every_glyph(uint32_t) { return true; }

static std::string safe(const std::string &text, size_t max_bytes = 72) {
  return panel_safe(text, max_bytes, font_stand_in);
}

// upstream's extract_unicode_codepoint (components/font/font.cpp, ESPHome 2026.9.0), static there
// and so copied: the oracle for what Font::print draws. A zero length stops the row.
static uint32_t font_decode(const char *utf8_str, size_t *length) {
  const auto *current = reinterpret_cast<const uint8_t *>(utf8_str);
  uint32_t code_point = 0;
  uint8_t c1 = *current++;
  if (c1 == 0) {
    *length = 0;
    return 0;
  }
  if (c1 < 0x80) {
    code_point = c1;
  } else if ((c1 & 0xE0) == 0xC0) {
    uint8_t c2 = *current++;
    if ((c2 & 0xC0) != 0x80) {
      *length = 0;
      return 0;
    }
    code_point = (c1 & 0x1F) << 6;
    code_point |= (c2 & 0x3F);
    if (code_point <= 0x7F) {
      *length = 0;
      return 0;
    }
  } else if ((c1 & 0xF0) == 0xE0) {
    uint8_t c2 = *current++;
    uint8_t c3 = *current++;
    if (((c2 & 0xC0) != 0x80) || ((c3 & 0xC0) != 0x80)) {
      *length = 0;
      return 0;
    }
    code_point = (c1 & 0x0F) << 12;
    code_point |= (c2 & 0x3F) << 6;
    code_point |= (c3 & 0x3F);
    if (code_point <= 0x7FF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
      *length = 0;
      return 0;
    }
  } else if ((c1 & 0xF8) == 0xF0) {
    uint8_t c2 = *current++;
    uint8_t c3 = *current++;
    uint8_t c4 = *current++;
    if (((c2 & 0xC0) != 0x80) || ((c3 & 0xC0) != 0x80) || ((c4 & 0xC0) != 0x80)) {
      *length = 0;
      return 0;
    }
    code_point = (c1 & 0x07) << 18;
    code_point |= (c2 & 0x3F) << 12;
    code_point |= (c3 & 0x3F) << 6;
    code_point |= (c4 & 0x3F);
    if (code_point <= 0xFFFF || code_point > 0x10FFFF) {
      *length = 0;
      return 0;
    }
  } else {
    *length = 0;
    return 0;
  }
  *length = current - reinterpret_cast<const uint8_t *>(utf8_str);
  return code_point;
}

// The font reads the whole text, every code point one it has. It reads up to three bytes past a
// lead at the end, hence the padding.
static ::testing::AssertionResult the_font_draws_all_of(const std::string &text, bool (*has_glyph)(uint32_t)) {
  std::vector<char> padded(text.begin(), text.end());
  padded.resize(text.size() + 4, '\0');
  size_t at = 0;
  while (at < text.size()) {
    size_t length;
    const uint32_t code_point = font_decode(padded.data() + at, &length);
    if (length == 0)
      return ::testing::AssertionFailure() << "the row stops at byte " << at;
    if (!has_glyph(code_point))
      return ::testing::AssertionFailure() << "no glyph for U+" << std::hex << code_point;
    at += length;
  }
  return ::testing::AssertionSuccess();
}

// --- panel_safe; expected values as CPython's "replace" decoding gives them, '?' for U+FFFD ---

TEST(PanelSafe, WhatTheFontDrawsPassesThrough) {
  EXPECT_EQ(safe("Kitchen: 21.5 °C"), "Kitchen: 21.5 °C");
  EXPECT_EQ(safe("Включён"), "Включён");
  EXPECT_EQ(safe("Привет, мир"), "Привет, мир");
  EXPECT_EQ(safe("µg/m³ m²"), "µg/m³ m²");
  EXPECT_EQ(safe(""), "");
}

TEST(PanelSafe, ACodePointTheFontLacksIsOneQuestionMark) {
  EXPECT_EQ(safe("日本"), "??");
  EXPECT_EQ(safe("\xF0\x9F\x98\x80 ok"), "? ok");  // an emoji, four bytes
  EXPECT_EQ(safe("caf\xC3\xA9 5"), "caf? 5");      // é
}

TEST(PanelSafe, AControlCharacterIsAQuestionMarkWhateverTheFont) {
  EXPECT_EQ(panel_safe("a\nb", 72, every_glyph), "a?b");
  EXPECT_EQ(panel_safe("a\tb\rc", 72, every_glyph), "a?b?c");
  EXPECT_EQ(panel_safe(std::string("\0x", 2), 72, every_glyph), "?x");
  EXPECT_EQ(panel_safe("\x7F", 72, every_glyph), "?");
  EXPECT_EQ(panel_safe("\xC2\x85", 72, every_glyph), "?");  // U+0085, a C1 control
  EXPECT_EQ(panel_safe("\xC2\xA0", 72, every_glyph), "\xC2\xA0");
}

// The forms the font's decoder refuses, which would blank the rest of the row.
TEST(PanelSafe, AnOverlongFormIsRefused) {
  EXPECT_EQ(panel_safe("\xC1\x81", 72, every_glyph), "??");  // an overlong 'A'
  EXPECT_EQ(panel_safe("\xC0\x80", 72, every_glyph), "??");
  EXPECT_EQ(panel_safe("\xE0\x80\xAF", 72, every_glyph), "???");
}

TEST(PanelSafe, ASurrogateIsRefused) {
  EXPECT_EQ(panel_safe("\xED\xA0\x80", 72, every_glyph), "???");  // U+D800
  EXPECT_EQ(panel_safe("\xED\xBF\xBF\xED\xA0\x80"
                       "A",
                       72, every_glyph),
            "??????A");
}

TEST(PanelSafe, PastTheLastPlaneIsRefused) {
  EXPECT_EQ(panel_safe("\xF4\x90\x80\x80", 72, every_glyph), "????");  // U+110000
  EXPECT_EQ(panel_safe("\xF5\x80\x80\x80", 72, every_glyph), "????");
  EXPECT_EQ(panel_safe("\xF8\x88\x80\x80\x80", 72, every_glyph), "?????");
}

TEST(PanelSafe, AStrayByteIsOneQuestionMark) {
  EXPECT_EQ(safe("\x80"
                 "abc"),
            "?abc");
  EXPECT_EQ(safe("\xFF"), "?");
  EXPECT_EQ(safe("\xFE"), "?");
}

// One '?' per maximal ill-formed subpart: a sequence that breaks late keeps the bytes after it.
TEST(PanelSafe, ABrokenSequenceKeepsWhatFollowsIt) {
  EXPECT_EQ(safe("a\xE2\x82"
                 "b"),
            "a?b");
  EXPECT_EQ(safe("\xE2\x82"
                 "A"),
            "?A");
  EXPECT_EQ(safe("\xF0\x9F\x98"
                 "Z"),
            "?Z");
  EXPECT_EQ(safe("Привет, мир\xC3("), "Привет, мир?(");
  EXPECT_EQ(safe("ab\xE2\x82"), "ab?");  // at the very end
  EXPECT_EQ(safe("\xC2"), "?");
}

TEST(PanelSafe, TheEdgesOfEveryRangeAreWellFormed) {
  for (const char *text : {"\xC2\xA0", "\xDF\xBF", "\xE0\xA0\x80", "\xED\x9F\xBF", "\xEE\x80\x80", "\xF0\x90\x80\x80",
                           "\xF4\x8F\xBF\xBF"}) {
    EXPECT_EQ(panel_safe(text, 72, every_glyph), text);
  }
}

TEST(PanelSafe, OnlyTheFirstMaxBytesAreReadAndACutEndsInAnEllipsis) {
  EXPECT_EQ(safe("abcdef", 3), "abc…");
  EXPECT_EQ(safe("abc", 3), "abc");
  EXPECT_EQ(safe("abc", 0), "…");

  size_t asked = 0;
  const std::string kilobyte(1024, 'x');
  const std::string out = panel_safe(kilobyte, 72, [&asked](uint32_t) {
    asked++;
    return true;
  });
  EXPECT_EQ(out, std::string(72, 'x') + "…");
  EXPECT_EQ(asked, 72u);
}

// A code point the cut splits is the cut's, not a broken one.
TEST(PanelSafe, ACutInsideACodePointDropsIt) {
  EXPECT_EQ(safe("абв", 3), "а…");
  EXPECT_EQ(safe("абв", 4), "аб…");
  EXPECT_EQ(safe("a\xF0\x9F\x98\x80", 3), "a…");
  // Broken before the cut: still one '?'.
  EXPECT_EQ(safe("a\xE2"
                 "bcd",
                 3),
            "a?b…");
}

// Every one- and two-byte string, and three- and four-byte ones from the bytes where the rules
// change, come out as text the font reads to the end: with every glyph present, so what is
// well-formed reaches the font's decoder, and with the stand-in's.
TEST(PanelSafe, NothingItReturnsStopsTheFont) {
  const std::vector<uint8_t> edges = {0x00, 0x0A, 0x41, 0x7F, 0x80, 0x8F, 0x90, 0x9F, 0xA0, 0xBF, 0xC0, 0xFF};
  auto check = [](const std::string &text) {
    EXPECT_TRUE(the_font_draws_all_of(panel_safe(text, 72, every_glyph), every_glyph))
        << ::testing::PrintToString(text);
    EXPECT_TRUE(the_font_draws_all_of(panel_safe(text, 72, font_stand_in), font_stand_in))
        << ::testing::PrintToString(text);
  };
  auto byte = [](int value) { return static_cast<char>(value); };
  for (int a = 0; a < 256; a++) {
    check(std::string{byte(a)});
    for (int b = 0; b < 256; b++)
      check(std::string{byte(a), byte(b)});
  }
  for (int a = 0x80; a < 256; a++) {
    for (uint8_t b : edges) {
      for (uint8_t c : edges) {
        check(std::string{byte(a), byte(b), byte(c)});
        for (uint8_t d : edges)
          check(std::string{byte(a), byte(b), byte(c), byte(d)});
      }
    }
  }
}

// --- panel_glyphs, panel_fit ---

TEST(PanelGlyphs, CountsCodePointsNotBytes) {
  EXPECT_EQ(panel_glyphs(""), 0u);
  EXPECT_EQ(panel_glyphs("abc"), 3u);
  EXPECT_EQ(panel_glyphs("Включён"), 7u);
  EXPECT_EQ(panel_glyphs("21.5 °C"), 7u);
  EXPECT_EQ(panel_glyphs("…"), 1u);
  EXPECT_EQ(panel_glyphs("\xF0\x9F\x98\x80"), 1u);
}

TEST(PanelFit, ATextThatFitsIsKeptWhole) {
  EXPECT_EQ(panel_fit("10.0.2.2", 18), "10.0.2.2");
  EXPECT_EQ(panel_fit("exactly-eighteen-x", 18), "exactly-eighteen-x");
  EXPECT_EQ(panel_fit("", 0), "");
}

TEST(PanelFit, ALongerOneIsCutWithAnEllipsisAsItsLastGlyph) {
  const std::string fitted = panel_fit("Kitchen temperature sensor", 18);
  EXPECT_EQ(fitted, "Kitchen temperatu…");
  EXPECT_EQ(panel_glyphs(fitted), 18u);
  EXPECT_EQ(panel_fit("abc", 1), "…");
  EXPECT_EQ(panel_fit("abc", 0), "");
}

TEST(PanelFit, NoSpaceIsLeftBeforeTheEllipsis) {
  EXPECT_EQ(panel_fit("Outdoor temperature", 9), "Outdoor…");
  EXPECT_EQ(panel_fit("a   b c", 4), "a…");
  EXPECT_EQ(panel_fit("     x", 3), "…");
}

TEST(PanelFit, CyrillicIsCutByGlyphs) {
  EXPECT_EQ(panel_fit("Температура в спальне", 10), "Температу…");
  EXPECT_EQ(panel_fit("Включён", 7), "Включён");
}

// --- panel_pair ---

TEST(PanelPair, APairThatFitsIsKeptWhole) {
  EXPECT_EQ(panel_pair("Outdoor", "21.5 °C", 18), "Outdoor: 21.5 °C");
  EXPECT_EQ(panel_pair("Door", "Off", 18), "Door: Off");
  EXPECT_EQ(panel_pair("Name of nine", "--", 18), "Name of nine: --");
  EXPECT_EQ(panel_pair("Ворота", "Включён", 18), "Ворота: Включён");
  const std::string full = panel_pair("Exactly", "eighteen!", 18);
  EXPECT_EQ(panel_glyphs(full), 18u);
  EXPECT_EQ(full, "Exactly: eighteen!");
}

TEST(PanelPair, ALongNameMakesRoomForTheValue) {
  const std::string row = panel_pair("Outdoor temperature", "21.5 °C", 18);
  EXPECT_EQ(row, "Outdoor…: 21.5 °C");
  EXPECT_LE(panel_glyphs(row), 18u);
}

TEST(PanelPair, ALongValueLeavesTheNameSixGlyphs) {
  const std::string row = panel_pair("Kitchen sensor", "{\"temperature\":21.5,\"humidity\":40}", 18);
  EXPECT_EQ(row, "Kitch…: {\"tempera…");
  EXPECT_EQ(panel_glyphs(row), 18u);
}

TEST(PanelPair, AShortNameIsNeverPaddedOrCut) {
  const std::string row = panel_pair("Hall", "{\"temperature\":21.5,\"humidity\":40}", 18);
  EXPECT_EQ(row, "Hall: {\"temperatu…");
  EXPECT_EQ(panel_glyphs(row), 18u);
}

TEST(PanelPair, CyrillicCountsByGlyphs) {
  const std::string row = panel_pair("Температура в спальне", "21.5 °C", 18);
  EXPECT_EQ(row, "Температ…: 21.5 °C");
  EXPECT_EQ(panel_glyphs(row), 18u);
}

TEST(PanelPair, AnEmptyValueKeepsTheSeparator) {
  EXPECT_EQ(panel_pair("A name of twenty-two", "", 18), "A name of twent…: ");
}

TEST(PanelPair, ARowTooNarrowForTheSeparatorIsCutWhole) {
  EXPECT_EQ(panel_pair("Name", "1", 2), "N…");
  EXPECT_EQ(panel_pair("Name", "1", 0), "");
}

}  // namespace esphome::mqtt_config::testing
