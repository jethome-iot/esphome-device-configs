#include <gtest/gtest.h>

#include "common.h"

namespace esphome::display_menu_base::testing {

namespace {

/// Stands in for the menu font: printable ASCII plus a degree sign, no Cyrillic.
bool latin_only(uint32_t code_point) { return (code_point >= 0x20 && code_point <= 0x7E) || code_point == 0x00B0; }

/// A font that lacks nothing, so a case can weigh cutting apart from standing in.
bool draws_anything(uint32_t) { return true; }

std::string fit(const std::string &text, size_t chars) { return fit_text(text, chars, latin_only); }

/// esphome::font's own decoder, in the one respect that matters here: it hands Font::print a
/// zero length for a sequence it refuses, and print stops at the first of those.
bool the_font_can_draw_all_of(const std::string &text, const std::function<bool(uint32_t)> &can_draw) {
  size_t i = 0;
  while (i < text.size()) {
    const auto lead = static_cast<unsigned char>(text[i]);
    size_t len = lead < 0x80             ? 1
                 : (lead & 0xE0) == 0xC0 ? 2
                 : (lead & 0xF0) == 0xE0 ? 3
                 : (lead & 0xF8) == 0xF0 ? 4
                                         : 0;
    if (len == 0 || i + len > text.size() || lead == 0)
      return false;
    uint32_t code_point = lead;
    if (len > 1) {
      code_point = lead & (0xFF >> (len + 1));
      for (size_t k = 1; k < len; k++) {
        const auto next = static_cast<unsigned char>(text[i + k]);
        if ((next & 0xC0) != 0x80)
          return false;
        code_point = (code_point << 6) | (next & 0x3F);
      }
      static const uint32_t SHORTEST[] = {0, 0, 0x80, 0x800, 0x10000};
      if (code_point < SHORTEST[len] || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF))
        return false;
    }
    if (!can_draw(code_point))
      return false;
    i += len;
  }
  return true;
}

}  // namespace

TEST(FitText, AShortAsciiNameIsLeftAlone) {
  EXPECT_EQ(fit("Porch light", 12), "Porch light");
  EXPECT_EQ(fit("", 12), "");
}

TEST(FitText, ALongNameIsCutToTheCharacterBudget) {
  EXPECT_EQ(fit("A very long rule name", 12), "A very long ");
  EXPECT_EQ(fit("exactlytwelv", 12), "exactlytwelv");
  EXPECT_EQ(fit("Porch light", 0), "");
}

// Multi-byte code points count as one character each, or the budget would cut a name far
// shorter than the row can hold.
TEST(FitText, TheBudgetCountsCharactersNotBytes) {
  EXPECT_EQ(fit_text("°°°°", 4, draws_anything), "°°°°");  // 8 bytes, 4 characters
  EXPECT_EQ(fit_text("°°°°", 2, draws_anything), "°°");
}

TEST(FitText, ACodePointTheFontLacksStandsIn) {
  EXPECT_EQ(fit("Свет", 12), "????");
  EXPECT_EQ(fit("Light Свет", 12), "Light ????");
  EXPECT_EQ(fit("20°C", 12), "20°C");  // the font has this one
}

// The stand-in is one character wide, so it must not blow the budget either — and a character
// the font does have keeps itself, even between ones it does not.
TEST(FitText, StandingInStillRespectsTheBudget) {
  EXPECT_EQ(fit("Свет на крыльце", 6), "???? ?");
  EXPECT_EQ(fit("Свет на крыльце", 15), "???? ?? ???????");
}

// A hand-written file can hold anything; a cut or stray byte must not run off the end or
// swallow the rest of the name.
TEST(FitText, MalformedUtf8StandsIn) {
  EXPECT_EQ(fit_text("\xD0", 4, draws_anything), "?");           // a lead byte with nothing after it
  EXPECT_EQ(fit_text("\xD0\xA1\xD0", 4, draws_anything), "С?");  // one good pair, then a cut one
  EXPECT_EQ(fit("\xA1"
                "ok",
                4),
            "?ok");  // a stray continuation byte
  EXPECT_EQ(fit("\xFF"
                "ok",
                4),
            "?ok");                                  // not a lead byte at all
  EXPECT_EQ(fit(std::string("a\0b", 3), 4), "a?b");  // an embedded NUL
}

// The four-byte form is a real lead length, not something to treat as junk.
TEST(FitText, AFourByteCodePointIsOneCharacter) {
  EXPECT_EQ(fit_text("\xF0\x9F\x92\xA1x", 2, draws_anything), "\xF0\x9F\x92\xA1x");
  EXPECT_EQ(fit("\xF0\x9F\x92\xA1x", 2), "?x");  // the font has no emoji
}

// These decode to something the font has if the bytes are read naively, so a lenient reader
// would pass them straight through — and the font's own reader then draws nothing at all.
TEST(FitText, ASequenceTheFontWouldRefuseStandsInEvenWhenItDecodesToAGlyph) {
  EXPECT_EQ(fit("\xC1\x81"
                "Porch",
                12),
            "?Porch");  // overlong 'A'
  EXPECT_EQ(fit("\xE0\x81\x81"
                "Porch",
                12),
            "?Porch");  // overlong 'A' again, three bytes
  EXPECT_EQ(fit("\xC1\x41"
                "bc",
                12),
            "?Abc");                                                // a continuation byte that is not one
  EXPECT_EQ(fit_text("\xED\xA0\x80", 4, draws_anything), "?");      // half a surrogate pair
  EXPECT_EQ(fit_text("\xF7\xBF\xBF\xBF", 4, draws_anything), "?");  // past the last plane
}

// A sequence that starts well and breaks late is still one character's worth of name: it must
// not spend a row's budget on one `?` per byte it did carry.
TEST(FitText, ABrokenSequenceCostsOneCharacter) {
  EXPECT_EQ(fit_text("\xF0\x9F\x92"
                     "abc",
                     4, draws_anything),
            "?abc");                                        // three of four bytes, then a letter
  EXPECT_EQ(fit_text("\xE0\x81", 4, draws_anything), "?");  // two of three, then the end
  EXPECT_EQ(fit_text("\xF0\x9F", 1, draws_anything), "?");
}

// The point of the whole function: whatever comes out, the font draws all of it.
TEST(FitText, EverythingItReturnsIsDrawable) {
  const std::vector<std::string> nasty{
      "Свет на крыльце",
      "\xC1\x81",
      "\xE0\x81\x81",
      "\xED\xA0\x80",
      "\xF7\xBF\xBF\xBF",
      "\xF0\x9F\x92\xA1",
      "\xFF\xFE",
      "\xC1\x41"
      "bc",
      "\xD0",
      std::string("a\0b", 3),
      "20°C",
      "Porch light",
      "",
  };
  for (const auto &name : nasty) {
    for (size_t budget : {0u, 1u, 6u, 12u, 64u})
      EXPECT_TRUE(the_font_can_draw_all_of(fit_text(name, budget, latin_only), latin_only))
          << "budget " << budget << " of " << ::testing::PrintToString(name);
  }
}

// The Automations rows label themselves this way: what fits keeps itself, and what the row
// could not show as written carries the rule's id, since it may no longer read uniquely. This
// pins that shape, the way test_toggle_row pins the toggle's.
namespace {

std::string automations_label(const std::string &name, uint32_t id) {
  const std::string cut = fit_text(name, 12, latin_only);
  if (cut == name)
    return cut;
  const std::string tag = " #" + std::to_string(id);
  return fit_text(name, 12 - tag.size(), latin_only) + tag;
}

size_t characters(const std::string &text) { return fit_text(text, 99, draws_anything).size(); }

}  // namespace

TEST(AutomationsLabel, ANameThatFitsCarriesNoId) {
  EXPECT_EQ(automations_label("Porch light", 1), "Porch light");
  EXPECT_EQ(automations_label("exactlytwelv", 9), "exactlytwelv");
}

TEST(AutomationsLabel, ACutNameCarriesItsId) {
  EXPECT_EQ(automations_label("Porch light A", 1), "Porch lig #1");
  EXPECT_EQ(automations_label("Porch light B", 2), "Porch lig #2");
}

// Standing in costs uniqueness too: two different names can both read as question marks.
TEST(AutomationsLabel, ANameThatStoodInCarriesItsIdEvenWhenItFits) {
  EXPECT_EQ(automations_label("Свет", 7), "???? #7");
  EXPECT_NE(automations_label("Свет", 7), automations_label("Тень", 8));
}

// The row is 18 characters and the value takes six of them.
TEST(AutomationsLabel, TheLabelNeverOutgrowsTheRow) {
  const uint32_t widest_id = (1u << 28) - 1;  // MAX_RULE_ID in components/automations
  for (const auto &name : {"Porch light A", "Свет на крыльце", "A very long rule name indeed", "x"})
    for (uint32_t id : {1u, 255u, widest_id})
      EXPECT_LE(characters(automations_label(name, id)), 12u) << name << " #" << id;
}

}  // namespace esphome::display_menu_base::testing
