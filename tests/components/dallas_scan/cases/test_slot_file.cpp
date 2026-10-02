#include "common.h"

namespace esphome::dallas_scan::testing {

// --- parse_address ---

TEST(ParseAddress, TakesZeroXAndOneToSixteenHexDigits) {
  struct Case {
    const char *text;
    uint64_t value;
  };
  const std::vector<Case> cases = {
      {"0xeb01227905460228", ROM_A},
      {"0XEB01227905460228", ROM_A},
      {"0xEb01227905460228", ROM_A},
      {"0x28", 0x28},
      {"0x1", 1},
      {"0x0000000000000028", 0x28},  // padded to 16, as the file is written
      {"0xffffffffffffffff", ~uint64_t{0}},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.text);
    uint64_t address = 0;
    EXPECT_TRUE(parse_address(c.text, address));
    EXPECT_EQ(address, c.value);
  }
}

TEST(ParseAddress, RefusesAnythingElseAndLeavesTheAddressAlone) {
  const std::vector<const char *> cases = {
      "",
      "0",
      "0x",
      "0x0",
      "0x0000000000000000",
      "0x00000000000000028",  // 17 digits, though the value would fit
      "0x1eb01227905460228",
      "eb01227905460228",
      "x28",
      "00x28",
      " 0x28",
      "0x 28",
      "0x28 ",
      "0x28g",
      "0x-28",
      "-0x28",
      "0b101",
  };
  for (const char *text : cases) {
    SCOPED_TRACE(text);
    uint64_t address = ROM_B;
    EXPECT_FALSE(parse_address(text, address));
    EXPECT_EQ(address, ROM_B);
  }
  uint64_t address = ROM_B;
  EXPECT_FALSE(parse_address(nullptr, address));
  EXPECT_EQ(address, ROM_B);
}

// --- parse_json and write_json ---

// A slot file of `slots` slots after parsing `text`, and what parse_json returned.
struct Parsed {
  bool ok;
  std::vector<uint64_t> table;
};

inline Parsed parse(const std::string &text, size_t slots = 4) {
  JsonDocument doc;
  EXPECT_EQ(deserializeJson(doc, text), DeserializationError::Ok) << text;
  SlotFile file("dallas_scan_temps", slots);
  const bool ok = file.parse_json(doc.as<JsonObject>(), 1);
  return {ok, file.table()};
}

inline std::string written(const std::vector<uint64_t> &table) {
  SlotFile file("dallas_scan_temps", table.size());
  file.set_table(table);
  JsonDocument doc;
  file.write_json(doc.to<JsonObject>(), 1);
  std::string out;
  serializeJson(doc, out);
  return out;
}

class SlotFileParse : public ::testing::Test {
 protected:
  void SetUp() override { LogCapture::instance().clear(); }
  static LogCapture &log() { return LogCapture::instance(); }
};

TEST_F(SlotFileParse, PutsEachAddressInItsSlot) {
  const Parsed parsed = parse(R"({"version":1,"records":[{"slot":3,"address":"0xeb01227905460228"},)"
                              R"({"slot":1,"address":"0x8a0122791699dd28"}]})");
  EXPECT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.table, (std::vector<uint64_t>{ROM_B, 0, ROM_A, 0}));
  EXPECT_TRUE(log().warnings.empty());
}

TEST_F(SlotFileParse, AnEmptyListIsAnEmptyTable) {
  const Parsed parsed = parse(R"({"version":1,"records":[]})");
  EXPECT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.table, (std::vector<uint64_t>{0, 0, 0, 0}));
}

TEST_F(SlotFileParse, RecordsThatAreNotAListFailTheFile) {
  for (const char *text : {R"({"version":1})", R"({"version":1,"records":{"slot":1}})", R"({"records":null})"}) {
    SCOPED_TRACE(text);
    log().clear();
    const Parsed parsed = parse(text);
    EXPECT_FALSE(parsed.ok);
    EXPECT_EQ(parsed.table, (std::vector<uint64_t>{0, 0, 0, 0}));
    EXPECT_TRUE(log().has(log().errors, "'records' must be an array"));
  }
}

TEST_F(SlotFileParse, ARecordWithoutAnIntegerSlotFromOneUpIsSkipped) {
  const Parsed parsed = parse(R"({"records":[)"
                              R"({"address":"0x3c01b5566e8a1f28"},)"
                              R"({"slot":0,"address":"0x3c01b5566e8a1f28"},)"
                              R"({"slot":-1,"address":"0x3c01b5566e8a1f28"},)"
                              R"({"slot":"2","address":"0x3c01b5566e8a1f28"},)"
                              R"({"slot":2.5,"address":"0x3c01b5566e8a1f28"},)"
                              R"({"slot":true,"address":"0x3c01b5566e8a1f28"},)"
                              R"({"slot":null,"address":"0x3c01b5566e8a1f28"},)"
                              R"({"slot":4294967297,"address":"0x3c01b5566e8a1f28"},)"
                              R"({"slot":1,"address":"0xeb01227905460228"}]})");
  EXPECT_TRUE(parsed.ok);  // a bad record does not fail the file
  EXPECT_EQ(parsed.table, (std::vector<uint64_t>{ROM_A, 0, 0, 0}));
  EXPECT_EQ(
      std::count_if(log().warnings.begin(), log().warnings.end(),
                    [](const std::string &line) { return line.find("without a valid slot") != std::string::npos; }),
      8);
}

TEST_F(SlotFileParse, ASlotPastMaxSensorsIsSkippedWithoutFailingTheFile) {
  const Parsed parsed = parse(R"({"records":[{"slot":5,"address":"0x3c01b5566e8a1f28"},)"
                              R"({"slot":4,"address":"0xeb01227905460228"}]})");
  EXPECT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.table, (std::vector<uint64_t>{0, 0, 0, ROM_A}));
  EXPECT_TRUE(log().has(log().warnings, "Slot 5 is past max_sensors"));
}

TEST_F(SlotFileParse, ARecordWithoutAHexStringAddressIsSkipped) {
  const Parsed parsed = parse(R"({"records":[)"
                              R"({"slot":1},)"
                              R"({"slot":1,"address":null},)"
                              R"({"slot":1,"address":16933853977064636968},)"  // ROM_A as a number
                              R"({"slot":1,"address":"0x0"},)"
                              R"({"slot":1,"address":"eb01227905460228"},)"
                              R"({"slot":1,"address":"0x00000000000000028"},)"
                              R"({"slot":2,"address":"0xeb01227905460228"}]})");
  EXPECT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.table, (std::vector<uint64_t>{0, ROM_A, 0, 0}));
  EXPECT_TRUE(log().has(log().warnings, "Slot 1: the address is not a \"0x...\" hex string"));
}

TEST_F(SlotFileParse, OfTwoRecordsForOneSlotTheLaterWins) {
  const Parsed parsed = parse(R"({"records":[{"slot":1,"address":"0xeb01227905460228"},)"
                              R"({"slot":1,"address":"0x8a0122791699dd28"}]})");
  EXPECT_EQ(parsed.table, (std::vector<uint64_t>{ROM_B, 0, 0, 0}));
  EXPECT_TRUE(log().has(log().warnings, "Slot 1 is listed twice"));
}

TEST_F(SlotFileParse, OfTwoRecordsForOneAddressTheLaterWins) {
  const Parsed parsed = parse(R"({"records":[{"slot":1,"address":"0xeb01227905460228"},)"
                              R"({"slot":3,"address":"0xEB01227905460228"}]})");
  EXPECT_EQ(parsed.table, (std::vector<uint64_t>{0, 0, ROM_A, 0}));
  EXPECT_TRUE(log().has(log().warnings, "0xeb01227905460228 is listed twice, keeping slot 3"));
}

TEST_F(SlotFileParse, ARecordThatClashesOnBothTakesBothPlaces) {
  // B leaves slot 2 for slot 1, and A, which held slot 1, is gone.
  const Parsed parsed = parse(R"({"records":[{"slot":1,"address":"0xeb01227905460228"},)"
                              R"({"slot":2,"address":"0x8a0122791699dd28"},)"
                              R"({"slot":1,"address":"0x8a0122791699dd28"}]})");
  EXPECT_EQ(parsed.table, (std::vector<uint64_t>{ROM_B, 0, 0, 0}));
}

TEST_F(SlotFileParse, AParseStartsFromAnEmptyTable) {
  JsonDocument doc;
  SlotFile file("dallas_scan_temps", 2);
  file.set_table({ROM_A, ROM_B});
  deserializeJson(doc, R"({"records":[{"slot":2,"address":"0x3c01b5566e8a1f28"}]})");
  EXPECT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
  EXPECT_EQ(file.table(), (std::vector<uint64_t>{0, ROM_C}));
  // A file that fails leaves nothing of the table before it either.
  file.set_table({ROM_A, ROM_B});
  deserializeJson(doc, R"({"records":7})");
  EXPECT_FALSE(file.parse_json(doc.as<JsonObject>(), 1));
  EXPECT_EQ(file.table(), (std::vector<uint64_t>{0, 0}));
}

TEST(SlotFileWrite, WritesOnlyBoundSlotsAsPaddedLowercaseHex) {
  EXPECT_EQ(written({0, 0x28, 0, ROM_A}), R"({"records":[{"slot":2,"address":"0x0000000000000028"},)"
                                          R"({"slot":4,"address":"0xeb01227905460228"}]})");
  EXPECT_EQ(written({0, 0, 0}), R"({"records":[]})");
}

TEST(SlotFileWrite, WhatIsWrittenParsesBackToTheSameTable) {
  const std::vector<uint64_t> table = {ROM_C, 0, ROM_A, ROM_B, 0, 0x10};
  const Parsed parsed = parse(written(table), table.size());
  EXPECT_TRUE(parsed.ok);
  EXPECT_EQ(parsed.table, table);
}

TEST(SlotFile, SizeCountsTheBoundSlotsAndResetEmptiesThem) {
  SlotFile file("dallas_scan_temps", 4);
  EXPECT_STREQ(file.get_key(), "dallas_scan_temps");
  EXPECT_EQ(file.size(), 0u);
  file.set_table({ROM_A, 0, ROM_B, 0});
  EXPECT_EQ(file.size(), 2u);
  file.apply();  // nothing to push: the scan reads the table in its own setup
  EXPECT_EQ(file.table(), (std::vector<uint64_t>{ROM_A, 0, ROM_B, 0}));
  file.reset();
  EXPECT_EQ(file.size(), 0u);
  EXPECT_EQ(file.table(), (std::vector<uint64_t>{0, 0, 0, 0}));
}

}  // namespace esphome::dallas_scan::testing
