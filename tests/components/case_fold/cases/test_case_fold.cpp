#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "esphome/components/case_fold/case_fold.h"

namespace esphome::case_fold::testing {

// scripts/build-case-fold.py's output, next to test.yaml: run.py runs the suite from there. The
// dashboard holds the clients' table to the same file.
static const char *const FIXTURE = "fold_cases.tsv";

static std::map<uint32_t, uint32_t> read_fixture() {
  std::map<uint32_t, uint32_t> rows;
  std::ifstream in(FIXTURE);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream fields(line);
    std::string from, to;
    std::getline(fields, from, '\t');
    std::getline(fields, to, '\t');
    rows[std::stoul(from, nullptr, 16)] = std::stoul(to, nullptr, 16);
  }
  return rows;
}

static std::string utf8(uint32_t c) {
  std::string out;
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
  return out;
}

static std::string hex(uint32_t c) {
  char text[16];
  snprintf(text, sizeof(text), "U+%04X", static_cast<unsigned>(c));
  return text;
}

TEST(CaseFold, TheFixtureIsThere) {
  const auto rows = read_fixture();
  size_t folds = 0;
  for (const auto &[from, to] : rows)
    folds += from != to;
  // A–Z and the 537 capitals of the scripts; a short file is a broken checkout, not a pass.
  EXPECT_EQ(563u, folds);
  EXPECT_GT(rows.size(), folds) << "and the code points that must stay";
}

TEST(CaseFold, EveryRowOfTheFixtureFoldsAsItSays) {
  for (const auto &[from, to] : read_fixture()) {
    EXPECT_EQ(to, fold_code_point(from)) << hex(from);
    EXPECT_EQ(utf8(to), fold_case(utf8(from))) << hex(from);
  }
}

// The fixture lists every fold, so a code point it does not fold stays, whatever the range.
TEST(CaseFold, EveryOtherCodePointStaysAsItIs) {
  const auto rows = read_fixture();
  for (uint32_t c = 0; c <= 0x10FFFF; c++) {
    if (c >= 0xD800 && c <= 0xDFFF)
      continue;
    const auto row = rows.find(c);
    if (row != rows.end() && row->second != c)
      continue;
    ASSERT_EQ(c, fold_code_point(c)) << hex(c);
    ASSERT_EQ(utf8(c), fold_case(utf8(c))) << hex(c);
  }
}

TEST(CaseFold, NamesAlikeButForCaseFoldAlike) {
  const std::pair<const char *, const char *> alike[] = {
      {"Kitchen", "KITCHEN"}, {"Кухня", "КУХНЯ"},   {"Café crème", "CAFÉ CRÈME"}, {"Οδός", "ΟΔΌΣ"}, {"Ёлка", "ёЛКА"},
      {"Żółć", "ŻÓŁĆ"},       {"Straße", "STRAẞE"}, {"Ǆemal", "ǅEMAL"},           {"Ⱥ", "ⱥ"},
  };
  for (const auto &[one, other] : alike)
    EXPECT_EQ(fold_case(one), fold_case(other)) << one << " / " << other;
  EXPECT_EQ("кухня", fold_case("КУХНЯ"));
  EXPECT_EQ("οδοσ", fold_case("ΟΔΟΣ"));
  EXPECT_EQ("οδοσ", fold_case("οδος")) << "final sigma is sigma";
}

TEST(CaseFold, WhatIsNotALetterOfTheScriptsStays) {
  EXPECT_EQ("", fold_case(""));
  EXPECT_EQ("room 1 (south) _-/\\", fold_case("ROOM 1 (South) _-/\\"));
  EXPECT_EQ("\xC2\xB5", fold_case("\xC2\xB5")) << "MICRO SIGN, not Greek mu";
  EXPECT_EQ("\xC5\xBF", fold_case("\xC5\xBF")) << "LONG S, not s";
  EXPECT_EQ("\xC4\xB0", fold_case("\xC4\xB0")) << "I WITH DOT ABOVE";
  EXPECT_EQ("\xE2\x84\xAA", fold_case("\xE2\x84\xAA")) << "KELVIN SIGN";
  EXPECT_EQ("\xE1\xBC\x88", fold_case("\xE1\xBC\x88")) << "Greek Extended";
  EXPECT_EQ("\xF0\x9F\x94\xA5 \xF0\x90\x90\x80", fold_case("\xF0\x9F\x94\xA5 \xF0\x90\x90\x80"));
}

// Two bytes become three: what a cut by bytes after the fold must allow for.
TEST(CaseFold, AFoldMayTakeMoreBytes) {
  EXPECT_EQ("\xE2\xB1\xA5", fold_case("\xC8\xBA"));
  EXPECT_EQ("\xC3\x9F", fold_case("\xE1\xBA\x9E")) << "and fewer";
}

// A name is valid UTF-8 by the time it is compared, but a YAML climate's need not have been
// checked: a broken sequence is kept byte for byte, and the letters around it still fold.
TEST(CaseFold, BytesThatAreNotUtf8StayAsTheyAre) {
  const char *const broken[] = {
      "\xD0",              // a lead byte at the end
      "\xD0\x28",          // a lead byte without its continuation
      "\x9A",              // a continuation byte alone
      "\xC0\x80",          // overlong
      "\xC1\xBF",          // overlong
      "\xE0\x80\x80",      // overlong
      "\xE0\x9F\xBF",      // overlong
      "\xC1\x81",          // A, overlong
      "\xE0\x90\x96",      // Ж, overlong
      "\xED\xA0\x80",      // a surrogate
      "\xE1\xBA",          // three bytes cut short
      "\xF0\x9F\x94",      // four bytes cut short
      "\xF5\x80\x80\x80",  // past U+10FFFF
      "\xFF",
  };
  for (const char *text : broken)
    EXPECT_EQ(std::string(text), fold_case(text)) << hex(static_cast<uint8_t>(text[0]));
  EXPECT_EQ("\xFF\xD0\xBA\xD0", fold_case("\xFF\xD0\x9A\xD0"));
  EXPECT_EQ("\xE1\xBA\xD0\xBA", fold_case("\xE1\xBA\xD0\x9A"));
  EXPECT_EQ(std::string("a\0\xD0\xBA", 4), fold_case(std::string("A\0\xD0\x9A", 4))) << "NUL is a byte too";
  // A view that ends inside a letter: what follows it is not the letter's.
  EXPECT_EQ("\xD0", fold_case(std::string_view("\xD0\x9A", 1)));
  EXPECT_EQ("\xE1\xBA", fold_case(std::string_view("\xE1\xBA\x9E", 2)));
}

}  // namespace esphome::case_fold::testing
