#include "common.h"
#include <memory>

namespace esphome::dallas_scan::testing {

static const char *const LABEL_HEX_A = "0xeb01227905460228";
static const char *const LABEL_HEX_B = "0x8a0122791699dd28";
static const char *const LABEL_HEX_C = "0x9b01b5566e8a1f28";

class Labels : public Boots {};

static Listing listed_c() {
  return [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_C);
  };
}

// The slot file with {"slot": N, "label": "text"} entries after the records and offsets; `text`
// goes in as written, so it must need no escaping.
static std::string with_labels(const std::string &file, const std::vector<std::pair<int, const char *>> &labels) {
  std::string out = file.substr(0, file.size() - 1) + R"(,"labels":[)";
  for (size_t i = 0; i < labels.size(); i++) {
    if (i > 0)
      out += ",";
    out += R"({"slot":)" + std::to_string(labels[i].first) + R"(,"label":")" + labels[i].second + R"("})";
  }
  return out + "]}";
}

static std::string cyrillic(size_t letters) {
  std::string out;
  for (size_t i = 0; i < letters; i++)
    out += "ж";
  return out;
}

// --- what a label may be and go on ---

TEST_F(Labels, CheckLabelGivesEveryCode) {
  TestScan &scan = this->boot({ROM_A}, 4, listed_c());
  EXPECT_EQ(scan.check_label(4, "Boiler"), LabelCheck::BAD_SLOT);
  EXPECT_EQ(scan.check_label(1000, "Boiler"), LabelCheck::BAD_SLOT);
  EXPECT_EQ(scan.check_label(1, "a\nb"), LabelCheck::BAD_TEXT);
  EXPECT_EQ(scan.check_label(1, std::string(25, 'x')), LabelCheck::BAD_TEXT);
  EXPECT_EQ(scan.check_label(1, std::string("a\0b", 3)), LabelCheck::BAD_TEXT);
  EXPECT_EQ(scan.check_label(1, "Свет\xD0"), LabelCheck::BAD_TEXT);
  EXPECT_EQ(scan.check_label(0, "Boiler"), LabelCheck::LISTED_SLOT);
  EXPECT_EQ(scan.check_label(1, "Boiler"), LabelCheck::OK);
  EXPECT_EQ(scan.check_label(1, cyrillic(24)), LabelCheck::OK);
  EXPECT_EQ(scan.check_label(1, ""), LabelCheck::OK);
  EXPECT_EQ(scan.check_label(3, "Boiler"), LabelCheck::OK);  // free: it waits for a sensor
  // In the order check_offset() checks.
  EXPECT_EQ(scan.check_label(4, "a\nb"), LabelCheck::BAD_SLOT);
  EXPECT_EQ(scan.check_label(0, "a\nb"), LabelCheck::BAD_TEXT);
}

TEST_F(Labels, ASlotHasNoLabelUntilOneIsSet) {
  TestScan &scan = this->boot({ROM_A});
  EXPECT_TRUE(scan.labels_supported());
  EXPECT_TRUE(scan.can_set_label());
  for (size_t slot : {0, 3, 4, 1000}) {
    EXPECT_EQ(scan.label(slot), "") << slot;
    EXPECT_EQ(scan.display_name(slot), scan.slot_name(slot)) << slot;
  }
  EXPECT_EQ(scan.display_name(0), "Temp 1");
  EXPECT_EQ(scan.display_name(3), "Temp 4");
}

// --- set_label_and_save ---

TEST_F(Labels, ALabelIsWrittenAndShownAtOnce) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  this->log().clear();
  EXPECT_TRUE(scan.set_label_and_save(1, "  Подача "));
  EXPECT_EQ(scan.label(1), "Подача");
  EXPECT_EQ(scan.display_name(1), "Подача");
  EXPECT_EQ(scan.slot_name(1), "Temp 2");  // the entity keeps its name
  EXPECT_EQ(scan.sensor(1)->get_name(), "Temp 2");
  EXPECT_TRUE(this->log().has(this->log().infos, "Temp 2: label \"Подача\""));
  EXPECT_EQ(this->read(), with_labels(slot_file({{1, LABEL_HEX_A}, {2, LABEL_HEX_B}}), {{2, "Подача"}}));
  EXPECT_EQ(this->boot({ROM_A, ROM_B}).label(1), "Подача");
}

TEST_F(Labels, AnEmptyLabelClearsIt) {
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_label_and_save(0, "Boiler"));
  this->log().clear();
  EXPECT_TRUE(scan.set_label_and_save(0, "   "));
  EXPECT_EQ(scan.label(0), "");
  EXPECT_EQ(scan.display_name(0), "Temp 1");
  EXPECT_TRUE(this->log().has(this->log().infos, "Temp 1: label cleared"));
  EXPECT_EQ(this->read(), slot_file({{1, LABEL_HEX_A}}));
}

TEST_F(Labels, TwentyFourCodePointsFitAndTwentyFiveDoNot) {
  TestScan &scan = this->boot({ROM_A});
  EXPECT_TRUE(scan.set_label_and_save(0, cyrillic(24)));
  EXPECT_EQ(scan.label(0), cyrillic(24));
  EXPECT_FALSE(scan.set_label_and_save(0, cyrillic(25)));
  EXPECT_EQ(scan.label(0), cyrillic(24));
}

// What the slot holds already writes nothing: a write would fail into the read-only folder.
TEST_F(Labels, TheLabelASlotHasAlreadyWritesNothing) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_label_and_save(0, "Boiler"));
  const std::string text = this->read();
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  this->log().clear();
  EXPECT_TRUE(scan.set_label_and_save(0, "Boiler"));
  EXPECT_TRUE(scan.set_label_and_save(0, " Boiler  "));  // trims to the same
  EXPECT_TRUE(scan.set_label_and_save(1, ""));
  chmod(this->dir().c_str(), 0755);
  EXPECT_TRUE(this->log().errors.empty());
  EXPECT_TRUE(this->log().infos.empty());
  EXPECT_EQ(this->read(), text);
}

TEST_F(Labels, WhatCheckLabelRefusesChangesNothing) {
  TestScan &scan = this->boot({ROM_A, ROM_B}, 4, listed_c());
  const std::string text = this->read();
  this->log().clear();
  EXPECT_FALSE(scan.set_label_and_save(4, "Boiler"));
  EXPECT_FALSE(scan.set_label_and_save(1, "a\tb"));
  EXPECT_FALSE(scan.set_label_and_save(0, "Boiler"));  // listed
  EXPECT_TRUE(this->log().has(this->log().warnings, "Not setting the label of slot 5"));
  EXPECT_TRUE(this->log().has(this->log().warnings, "Not setting the label of slot 2"));
  EXPECT_TRUE(this->log().has(this->log().warnings, "Not setting the label of slot 1"));
  for (size_t slot = 0; slot < 4; slot++)
    EXPECT_EQ(scan.label(slot), "") << slot;
  EXPECT_EQ(this->read(), text);
}

TEST_F(Labels, AWriteThatFailsChangesNothing) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_label_and_save(0, "Boiler"));
  const std::string text = this->read();
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  this->log().clear();
  EXPECT_FALSE(scan.set_label_and_save(0, "Return"));
  EXPECT_FALSE(scan.set_label_and_save(0, ""));
  chmod(this->dir().c_str(), 0755);
  EXPECT_TRUE(this->log().has(this->log().errors, "The slot table was not written: the label is not changed"));
  EXPECT_FALSE(this->log().has(this->log().infos, "label"));
  EXPECT_EQ(scan.label(0), "Boiler");
  EXPECT_EQ(this->read(), text);
}

// A label rolled back with the rest: a failed table write after it takes back nothing it set.
TEST_F(Labels, AFailedWriteOfSomethingElseKeepsTheLabels) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_label_and_save(0, "Boiler"));
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  EXPECT_FALSE(scan.forget_and_save(-1));
  EXPECT_FALSE(scan.set_offset_and_save(0, 0.5f));
  EXPECT_FALSE(scan.assign_and_save(0, ROM_C));
  chmod(this->dir().c_str(), 0755);
  EXPECT_EQ(scan.label(0), "Boiler");
  EXPECT_EQ(scan.saved_address(0), ROM_A);
}

TEST_F(Labels, WithoutAMountNothingChanges) {
  const std::string text = slot_file({{1, LABEL_HEX_A}});
  this->write(text);
  TestScan &scan = this->boot({ROM_A}, 4, nullptr, false);
  this->log().clear();
  EXPECT_TRUE(scan.labels_supported());
  EXPECT_FALSE(scan.can_set_label());
  EXPECT_FALSE(scan.set_label_and_save(0, "Boiler"));
  EXPECT_TRUE(this->log().has(this->log().errors, "Storage unavailable: the label is not changed"));
  EXPECT_EQ(scan.label(0), "");
  EXPECT_TRUE(scan.set_label_and_save(0, ""));  // what it has: nothing to write
  EXPECT_EQ(this->read(), text);
}

// A file that did not load is left for a person to fix: no label is written over it this boot.
TEST_F(Labels, ALabelWaitsForASlotFileThatLoads) {
  const std::string text = R"({"version":1,"records":[{"slot":1,"address":"0x8a01)";
  this->write(text);
  TestScan &scan = this->boot({ROM_A});
  this->log().clear();
  EXPECT_TRUE(scan.can_save());
  EXPECT_FALSE(scan.can_set_label());
  EXPECT_FALSE(scan.set_label_and_save(0, "Boiler"));
  EXPECT_TRUE(this->log().has(this->log().errors, "The slot file did not load: the label is not changed"));
  EXPECT_EQ(scan.label(0), "");
  EXPECT_TRUE(scan.set_label_and_save(0, ""));
  EXPECT_EQ(this->read(), text);
  TestScan &after = this->boot({ROM_A});
  EXPECT_FALSE(after.can_set_label());  // still unreadable: nothing wrote over it
}

// No reading moves, nothing is published and nothing waits for a reboot.
TEST_F(Labels, ALabelPublishesNothingAndNeedsNoReboot) {
  TestScan &scan = this->boot({ROM_A});
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  auto published = std::make_shared<int>(0);
  scan.sensor(0)->add_on_state_callback([published](float) { (*published)++; });
  ASSERT_TRUE(scan.set_label_and_save(0, "Boiler"));
  ASSERT_TRUE(scan.set_label_and_save(1, "Return"));
  EXPECT_EQ(*published, 0);
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.0f);
  EXPECT_FALSE(scan.reboot_required());
  EXPECT_FALSE(scan.slot_pending(0));
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(Labels, AFreeSlotKeepsItsLabelForTheSensorThatTakesIt) {
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_label_and_save(2, "Return"));
  EXPECT_EQ(scan.sensor(2), nullptr);
  EXPECT_EQ(scan.display_name(2), "Return");
  EXPECT_EQ(this->read(), with_labels(slot_file({{1, LABEL_HEX_A}}), {{3, "Return"}}));
  TestScan &after = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(after.address(1), ROM_B);
  EXPECT_EQ(after.label(1), "");
  EXPECT_EQ(after.label(2), "Return");
  ASSERT_TRUE(after.assign_and_save(2, ROM_C));
  EXPECT_EQ(this->boot({ROM_A, ROM_B, ROM_C}).display_name(2), "Return");
}

// --- the slot number keeps it ---

TEST_F(Labels, AForgetOfOneSlotKeepsItsLabel) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_label_and_save(0, "Boiler"));
  ASSERT_TRUE(scan.forget_and_save(0));
  EXPECT_EQ(scan.label(0), "Boiler");
  EXPECT_EQ(this->read(), with_labels(slot_file({{2, LABEL_HEX_B}}), {{1, "Boiler"}}));
  EXPECT_FALSE(scan.can_forget(0));  // a label alone is not a device to forget
  scan.forget(0);
  EXPECT_EQ(scan.label(0), "Boiler");
  EXPECT_EQ(this->boot({ROM_B}).label(0), "Boiler");
}

TEST_F(Labels, AnAssignOrASwapLeavesTheLabelsOnTheirSlots) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_label_and_save(0, "Boiler"));
  ASSERT_TRUE(scan.set_label_and_save(1, "Return"));
  ASSERT_TRUE(scan.assign_and_save(0, ROM_B));  // a swap
  ASSERT_TRUE(scan.assign_and_save(2, ROM_C));
  EXPECT_EQ(scan.label(0), "Boiler");
  EXPECT_EQ(scan.label(1), "Return");
  EXPECT_EQ(scan.label(2), "");
  EXPECT_EQ(this->read(), with_labels(slot_file({{1, LABEL_HEX_B}, {2, LABEL_HEX_A}, {3, LABEL_HEX_C}}),
                                      {{1, "Boiler"}, {2, "Return"}}));
  TestScan &after = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(after.address(0), ROM_B);
  EXPECT_EQ(after.display_name(0), "Boiler");
}

TEST_F(Labels, ForgetAllClearsTheLabelsOfTheSlotsItEmpties) {
  TestScan &scan = this->boot({ROM_C, ROM_A}, 4, listed_c());
  ASSERT_TRUE(scan.set_label_and_save(1, "Boiler"));
  ASSERT_TRUE(scan.set_label_and_save(3, "Return"));  // free
  ASSERT_TRUE(scan.set_offset_and_save(1, 0.5f));
  ASSERT_TRUE(scan.forget_and_save(-1));
  for (size_t slot = 0; slot < 4; slot++)
    EXPECT_EQ(scan.label(slot), "") << slot;
  EXPECT_EQ(this->read(), slot_file({{1, LABEL_HEX_C}}));
}

// A table with nothing left but a label still has something to forget.
TEST_F(Labels, ForgetAllWithOnlyLabelsLeftClearsThem) {
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_label_and_save(2, "Return"));
  ASSERT_TRUE(scan.forget_and_save(0));
  EXPECT_FALSE(scan.can_forget(2));
  EXPECT_TRUE(scan.can_forget(-1));
  this->log().clear();
  scan.forget(-1);
  EXPECT_EQ(scan.restarts, 1);
  EXPECT_FALSE(this->log().has(this->log().infos, "Rebooting to apply"));
  EXPECT_EQ(scan.label(2), "");
  EXPECT_FALSE(scan.can_forget(-1));
  EXPECT_EQ(this->read(), slot_file({}));
}

TEST_F(Labels, AForgetAllWhoseWriteFailsKeepsTheLabels) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  TestScan &scan = this->boot({});
  ASSERT_TRUE(scan.set_label_and_save(0, "Boiler"));
  const std::string text = this->read();
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  this->log().clear();
  EXPECT_FALSE(scan.forget_and_save(-1));
  chmod(this->dir().c_str(), 0755);
  EXPECT_TRUE(this->log().has(this->log().errors, "The slot table was not written: nothing is forgotten"));
  EXPECT_EQ(scan.label(0), "Boiler");
  EXPECT_TRUE(scan.can_forget(-1));
  EXPECT_EQ(this->read(), text);
}

// --- listed slots ---

// A slot listed since the label was stored is named by its YAML; the next write drops the label.
TEST_F(Labels, AListedSlotDropsAStoredLabel) {
  this->write(with_labels(slot_file({{1, LABEL_HEX_C}, {2, LABEL_HEX_A}}), {{1, "Old"}, {2, "Boiler"}}));
  TestScan &scan = this->boot({ROM_C, ROM_A}, 4, listed_c());
  EXPECT_EQ(scan.label(0), "");
  EXPECT_EQ(scan.display_name(0), "Boiler");  // the listed sensor's own name
  EXPECT_EQ(scan.label(1), "Boiler");
  ASSERT_TRUE(scan.set_label_and_save(1, "Return"));
  EXPECT_EQ(this->read(), with_labels(slot_file({{1, LABEL_HEX_C}, {2, LABEL_HEX_A}}), {{2, "Return"}}));
}

TEST_F(Labels, AListedSlotsStoredLabelIsNothingToForget) {
  const std::string text = with_labels(slot_file({{1, LABEL_HEX_C}}), {{1, "Old"}});
  this->write(text);
  TestScan &scan = this->boot({ROM_C}, 4, listed_c());
  EXPECT_EQ(scan.label(0), "");
  EXPECT_FALSE(scan.can_forget(-1));
  EXPECT_FALSE(scan.forget_and_save(-1));
  EXPECT_EQ(this->read(), text);
}

TEST_F(Labels, DumpConfigListsTheLabels) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_label_and_save(2, "Подача"));
  ASSERT_TRUE(scan.set_label_and_save(1, "Boiler"));
  this->log().clear();
  scan.dump_config();
  EXPECT_TRUE(this->log().has(this->log().configs, "Temp 3 label: Подача"));
  EXPECT_TRUE(this->log().has(this->log().configs, "Temp 2 label: Boiler"));
  EXPECT_FALSE(this->log().has(this->log().configs, "Temp 1 label"));
}

// --- storage: nvs ---

TEST_F(Labels, PreferencesHaveNoLabels) {
  TestScan &scan = this->boot_nvs({ROM_A});
  EXPECT_FALSE(scan.labels_supported());
  EXPECT_FALSE(scan.can_set_label());
  EXPECT_TRUE(scan.can_set_offset());
  this->log().clear();
  EXPECT_FALSE(scan.set_label_and_save(0, "Boiler"));
  EXPECT_TRUE(this->log().has(this->log().errors, "Labels need storage: file: the label is not changed"));
  EXPECT_TRUE(scan.set_label_and_save(0, " "));  // no label is what it has
  EXPECT_EQ(scan.label(0), "");
  EXPECT_EQ(scan.display_name(0), "Temp 1");
  EXPECT_TRUE(this->files().empty());
}

// --- the slot file ---

// The labels a slot file of `slots` slots holds after parsing `text`.
static std::vector<std::string> parsed_labels(const std::string &text, size_t slots = 4) {
  JsonDocument doc;
  EXPECT_EQ(deserializeJson(doc, text), DeserializationError::Ok) << text;
  SlotFile file("dallas_scan_temps", slots);
  file.set_labels(std::vector<std::string>(slots, "stale"));  // a parse starts from none
  EXPECT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
  return file.labels();
}

static std::string written(const std::vector<uint64_t> &table, const std::vector<int16_t> &offsets,
                           const std::vector<std::string> &labels) {
  SlotFile file("dallas_scan_temps", table.size());
  file.set_table(table);
  file.set_offsets(offsets);
  file.set_labels(labels);
  JsonDocument doc;
  file.write_json(doc.to<JsonObject>(), 1);
  std::string out;
  serializeJson(doc, out);
  return out;
}

class LabelFile : public ::testing::Test {
 protected:
  void SetUp() override { LogCapture::instance().clear(); }
  static LogCapture &log() { return LogCapture::instance(); }
  static int count(const char *needle) {
    return std::count_if(log().warnings.begin(), log().warnings.end(),
                         [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }
};

using Strings = std::vector<std::string>;

TEST_F(LabelFile, PutsEachLabelOnItsSlot) {
  EXPECT_EQ(parsed_labels(R"({"records":[],"labels":[{"slot":3,"label":"Подача"},{"slot":1,"label":"Boiler"}]})"),
            (Strings{"Boiler", "", "Подача", ""}));
  EXPECT_TRUE(log().warnings.empty());
}

// A file from before labels, or one whose list is null.
TEST_F(LabelFile, NoListIsNoLabels) {
  EXPECT_EQ(parsed_labels(R"({"records":[{"slot":1,"address":"0xeb01227905460228"}]})"), Strings(4));
  EXPECT_EQ(parsed_labels(R"({"records":[],"offsets":[{"slot":1,"offset":0.5}]})"), Strings(4));
  EXPECT_EQ(parsed_labels(R"({"records":[],"labels":null})"), Strings(4));
  EXPECT_TRUE(log().warnings.empty());
}

// The table and the offsets are what matter: a bad list costs the labels alone.
TEST_F(LabelFile, AListThatIsNotAListIsIgnoredAndTheRestStays) {
  for (const char *labels : {R"({"slot":1,"label":"Boiler"})", "5", R"("Boiler")", "true"}) {
    SCOPED_TRACE(labels);
    log().clear();
    const std::string text = std::string(R"({"records":[{"slot":1,"address":"0xeb01227905460228"}],)") +
                             R"("offsets":[{"slot":1,"offset":0.5}],"labels":)" + labels + "}";
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, text), DeserializationError::Ok);
    SlotFile file("dallas_scan_temps", 4);
    EXPECT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
    EXPECT_EQ(file.table(), (std::vector<uint64_t>{ROM_A, 0, 0, 0}));
    EXPECT_EQ(file.offsets(), (std::vector<int16_t>{5, 0, 0, 0}));
    EXPECT_EQ(file.labels(), Strings(4));
    EXPECT_TRUE(log().has(log().warnings, "'labels' is not an array, ignoring it"));
  }
}

TEST_F(LabelFile, AnEntryWithoutAnIntegerSlotFromOneUpIsSkipped) {
  EXPECT_EQ(parsed_labels(R"({"records":[],"labels":[)"
                          R"({"label":"a"},)"
                          R"({"slot":0,"label":"a"},)"
                          R"({"slot":-1,"label":"a"},)"
                          R"({"slot":"2","label":"a"},)"
                          R"({"slot":2.5,"label":"a"},)"
                          R"({"slot":true,"label":"a"},)"
                          R"({"slot":null,"label":"a"},)"
                          R"(3,)"
                          R"({"slot":4,"label":"d"}]})"),
            (Strings{"", "", "", "d"}));
  EXPECT_EQ(count("A label without a valid slot"), 8);
}

TEST_F(LabelFile, AnEntryPastMaxSensorsIsSkipped) {
  EXPECT_EQ(parsed_labels(R"({"records":[],"labels":[{"slot":5,"label":"e"},{"slot":4,"label":"d"}]})"),
            (Strings{"", "", "", "d"}));
  EXPECT_TRUE(log().has(log().warnings, "Slot 5's label is past max_sensors"));
}

// What the route would refuse is skipped too, the escaped NUL included.
TEST_F(LabelFile, ALabelThatBreaksTheRulesIsSkipped) {
  const std::string long_label(25, 'x');
  EXPECT_EQ(parsed_labels(R"({"records":[],"labels":[)"
                          R"({"slot":1},)"
                          R"({"slot":1,"label":null},)"
                          R"({"slot":1,"label":5},)"
                          R"({"slot":1,"label":true},)"
                          R"({"slot":1,"label":["a"]},)"
                          R"({"slot":1,"label":"a\nb"},)"
                          R"({"slot":1,"label":"a\u0000b"},)"
                          R"({"slot":1,"label":"\u0085"},)"
                          R"({"slot":1,"label":")" +
                          long_label +
                          R"("},)"
                          R"({"slot":2,"label":"ok"}]})"),
            (Strings{"", "ok", "", ""}));
  EXPECT_EQ(count("Slot 1: the label is not text of at most 24 characters"), 9);
}

TEST_F(LabelFile, MalformedUtf8IsSkipped) {
  // ArduinoJson passes raw bytes through, so the file can hold what no route would write.
  std::string text = std::string(R"({"records":[],"labels":[{"slot":1,"label":")") + "\xD0" +
                     R"("},{"slot":2,"label":")" + "\xC0\x80" + R"("}]})";
  EXPECT_EQ(parsed_labels(text), Strings(4));
  EXPECT_EQ(count("the label is not text"), 2);
}

TEST_F(LabelFile, OfTwoEntriesForOneSlotTheLaterWins) {
  EXPECT_EQ(parsed_labels(R"({"records":[],"labels":[{"slot":2,"label":"First"},{"slot":2,"label":"Second"}]})"),
            (Strings{"", "Second", "", ""}));
  EXPECT_TRUE(log().has(log().warnings, "Slot 2's label is listed twice, keeping the last one"));
  // An empty later one clears it.
  EXPECT_EQ(parsed_labels(R"({"records":[],"labels":[{"slot":2,"label":"First"},{"slot":2,"label":""}]})"), Strings(4));
}

TEST_F(LabelFile, ALabelIsTrimmedOnLoad) {
  EXPECT_EQ(parsed_labels(R"({"records":[],"labels":[{"slot":1,"label":"  Boiler "},{"slot":2,"label":"   "}]})"),
            (Strings{"Boiler", "", "", ""}));
  // Counted after the trim: 24 letters between spaces fit.
  EXPECT_EQ(parsed_labels(R"({"records":[],"labels":[{"slot":1,"label":"  )" + cyrillic(24) + R"(  "}]})"),
            (Strings{cyrillic(24), "", "", ""}));
  EXPECT_TRUE(log().warnings.empty());
}

TEST(LabelFileWrite, WritesOnlyTheSlotsWithALabel) {
  EXPECT_EQ(written({ROM_A, 0, 0}, {0, -3, 0}, {"", "Подача", "Boiler"}),
            R"({"records":[{"slot":1,"address":"0xeb01227905460228"}],)"
            R"("offsets":[{"slot":2,"offset":-0.3}],)"
            R"("labels":[{"slot":2,"label":"Подача"},{"slot":3,"label":"Boiler"}]})");
  // None: the file is as firmware before labels writes it.
  EXPECT_EQ(written({ROM_A, 0}, {0, 5}, {"", ""}),
            R"({"records":[{"slot":1,"address":"0xeb01227905460228"}],"offsets":[{"slot":2,"offset":0.5}]})");
}

// Not only after the offsets: the list was once written only when a slot had an offset.
TEST(LabelFileWrite, LabelsAreWrittenWithoutOffsets) {
  EXPECT_EQ(written({ROM_A, 0}, {0, 0}, {"", "Boiler"}),
            R"({"records":[{"slot":1,"address":"0xeb01227905460228"}],"labels":[{"slot":2,"label":"Boiler"}]})");
  EXPECT_EQ(written({0}, {0}, {"Boiler"}), R"({"records":[],"labels":[{"slot":1,"label":"Boiler"}]})");
}

TEST(LabelFileWrite, WhatIsWrittenParsesBackToTheSameLabels) {
  const Strings labels = {"Boiler", "", "Подача", "Say \"hi\" \\ back", cyrillic(24), "\xF0\x9F\x92\xA1"};
  EXPECT_EQ(parsed_labels(written(std::vector<uint64_t>(6, 0), std::vector<int16_t>(6, 0), labels), 6), labels);
}

TEST(LabelFileWrite, ResetClearsTheLabels) {
  SlotFile file("dallas_scan_temps", 2);
  file.set_table({ROM_A, 0});
  file.set_labels({"Boiler", "Return"});
  EXPECT_EQ(file.size(), 1u);  // records only
  file.reset();
  EXPECT_EQ(file.labels(), Strings(2));
}

}  // namespace esphome::dallas_scan::testing
