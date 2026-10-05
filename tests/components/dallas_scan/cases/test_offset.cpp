#include "common.h"
#include <cstdio>
#include <cstring>

namespace esphome::dallas_scan::testing {

static const char *const OFFSET_HEX_A = "0xeb01227905460228";
static const char *const OFFSET_HEX_B = "0x8a0122791699dd28";
static const char *const OFFSET_HEX_C = "0x9b01b5566e8a1f28";

class Offsets : public Boots {};

static Listing listed_c() {
  return [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_C);
  };
}

// --- rounding and range ---

// The same as a double and as the float nearest to it, the way the HTTP route hands it over.
TEST(OffsetTenths, RoundsToTheNearestTenthHalvesAwayFromZero) {
  struct Case {
    double celsius;
    int16_t tenths;
  };
  const std::vector<Case> cases = {
      {0.0, 0},   {0.04, 0},    {-0.04, 0}, {0.05, 1},    {-0.05, -1}, {0.15, 2},  {-0.15, -2},
      {0.25, 3},  {-0.25, -3},  {0.35, 4},  {-0.3, -3},   {1.0, 10},   {5.0, 50},  {-5.0, -50},
      {5.04, 50}, {-5.04, -50}, {4.95, 50}, {-4.95, -50}, {4.94, 49},  {0.149, 1}, {-0.351, -4},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.celsius);
    int16_t tenths = 99;
    EXPECT_TRUE(offset_tenths(c.celsius, tenths));
    EXPECT_EQ(tenths, c.tenths);
    tenths = 99;
    EXPECT_TRUE(offset_tenths((float) c.celsius, tenths));
    EXPECT_EQ(tenths, c.tenths);
  }
}

TEST(OffsetTenths, RefusesWhatIsNotANumberOrPastTheRangeOnceRounded) {
  for (double celsius :
       {5.05, -5.05, 5.1, -5.1, 10.0, 1e30, -1e30, (double) NAN, (double) INFINITY, (double) -INFINITY}) {
    SCOPED_TRACE(celsius);
    int16_t tenths = 7;
    EXPECT_FALSE(offset_tenths(celsius, tenths));
    EXPECT_FALSE(offset_tenths((float) celsius, tenths));
    EXPECT_EQ(tenths, 7);
  }
  int16_t tenths = 7;
  EXPECT_FALSE(offset_tenths(1e300, tenths));
  EXPECT_EQ(tenths, 7);
}

TEST(OffsetTenths, TheRangeIsPublic) {
  EXPECT_FLOAT_EQ(DallasScan::MAX_OFFSET, 5.0f);
  EXPECT_FLOAT_EQ(DallasScan::OFFSET_STEP, 0.1f);
}

// --- what an offset may go on ---

TEST_F(Offsets, CheckOffsetGivesEveryCode) {
  TestScan &scan = this->boot({ROM_A}, 4, listed_c());
  EXPECT_EQ(scan.check_offset(4, 0.1f), OffsetCheck::BAD_SLOT);
  EXPECT_EQ(scan.check_offset(1000, 0.1f), OffsetCheck::BAD_SLOT);
  EXPECT_EQ(scan.check_offset(1, NAN), OffsetCheck::BAD_VALUE);
  EXPECT_EQ(scan.check_offset(1, INFINITY), OffsetCheck::BAD_VALUE);
  EXPECT_EQ(scan.check_offset(1, 5.1f), OffsetCheck::BAD_VALUE);
  EXPECT_EQ(scan.check_offset(1, -5.05f), OffsetCheck::BAD_VALUE);
  EXPECT_EQ(scan.check_offset(0, 0.1f), OffsetCheck::LISTED_SLOT);
  EXPECT_EQ(scan.check_offset(1, 0.3f), OffsetCheck::OK);
  EXPECT_EQ(scan.check_offset(1, 5.04f), OffsetCheck::OK);
  EXPECT_EQ(scan.check_offset(1, -5.0f), OffsetCheck::OK);
  EXPECT_EQ(scan.check_offset(1, 0.0f), OffsetCheck::OK);
  EXPECT_EQ(scan.check_offset(3, 0.3f), OffsetCheck::OK);  // free: it waits for a sensor
  // In the order check_assign() checks.
  EXPECT_EQ(scan.check_offset(4, NAN), OffsetCheck::BAD_SLOT);
  EXPECT_EQ(scan.check_offset(0, NAN), OffsetCheck::BAD_VALUE);
}

TEST_F(Offsets, ASlotHasNoOffsetUntilOneIsSet) {
  TestScan &scan = this->boot({ROM_A});
  for (size_t slot : {0, 3, 4, 1000})
    EXPECT_EQ(scan.offset(slot), 0.0f) << slot;
}

// --- set_offset_and_save ---

TEST_F(Offsets, AnOffsetIsWrittenAndAppliedAtOnce) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  ASSERT_FLOAT_EQ(scan.temperature(0), 20.0f);
  this->log().clear();

  EXPECT_TRUE(scan.set_offset_and_save(0, -0.3f));
  EXPECT_FLOAT_EQ(scan.offset(0), -0.3f);
  EXPECT_FLOAT_EQ(scan.temperature(0), 19.7f);  // published again, without waiting for a poll
  EXPECT_TRUE(this->log().has(this->log().infos, "Temp 1: offset -0.3 °C"));
  EXPECT_EQ(this->read(), slot_file({{1, OFFSET_HEX_A}, {2, OFFSET_HEX_B}}, {{1, "-0.3"}}));
  // Nothing waits for a reboot.
  EXPECT_FALSE(scan.reboot_required());
  EXPECT_FALSE(scan.slot_pending(0));
  EXPECT_EQ(scan.restarts, 0);

  this->bus.set_reading(ROM_A, 21.0f);
  scan.poll();
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.7f);
  EXPECT_FLOAT_EQ(this->boot({ROM_A, ROM_B}).offset(0), -0.3f);
}

TEST_F(Offsets, TheValueIsRoundedToTheStep) {
  TestScan &scan = this->boot({ROM_A});
  EXPECT_TRUE(scan.set_offset_and_save(0, 0.25f));
  EXPECT_FLOAT_EQ(scan.offset(0), 0.3f);
  EXPECT_TRUE(scan.set_offset_and_save(0, -5.04f));
  EXPECT_FLOAT_EQ(scan.offset(0), -5.0f);
  EXPECT_EQ(this->read(), slot_file({{1, OFFSET_HEX_A}}, {{1, "-5.0"}}));
}

// What the slot holds already writes nothing: a write would fail into the read-only folder.
TEST_F(Offsets, TheOffsetASlotHasAlreadyWritesNothing) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_offset_and_save(0, 0.3f));
  const std::string text = this->read();
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  this->log().clear();
  EXPECT_TRUE(scan.set_offset_and_save(0, 0.3f));
  EXPECT_TRUE(scan.set_offset_and_save(0, 0.34f));  // rounds to the same
  EXPECT_TRUE(scan.set_offset_and_save(1, 0.0f));
  chmod(this->dir().c_str(), 0755);
  EXPECT_TRUE(this->log().errors.empty());
  EXPECT_TRUE(this->log().infos.empty());
  EXPECT_EQ(this->read(), text);
}

TEST_F(Offsets, WhatCheckOffsetRefusesChangesNothing) {
  TestScan &scan = this->boot({ROM_A, ROM_B}, 4, listed_c());
  const std::string text = this->read();
  this->log().clear();
  EXPECT_FALSE(scan.set_offset_and_save(4, 0.1f));
  EXPECT_FALSE(scan.set_offset_and_save(1, NAN));
  EXPECT_FALSE(scan.set_offset_and_save(1, 5.1f));
  EXPECT_FALSE(scan.set_offset_and_save(0, 0.1f));  // listed
  EXPECT_TRUE(this->log().has(this->log().warnings, "Not setting the offset of slot 5 to 0.10"));
  EXPECT_TRUE(this->log().has(this->log().warnings, "Not setting the offset of slot 2 to nan"));
  EXPECT_TRUE(this->log().has(this->log().warnings, "Not setting the offset of slot 1 to 0.10"));
  for (size_t slot = 0; slot < 4; slot++)
    EXPECT_EQ(scan.offset(slot), 0.0f) << slot;
  EXPECT_EQ(this->read(), text);
}

TEST_F(Offsets, WithoutAMountNothingChanges) {
  const std::string text = slot_file({{1, OFFSET_HEX_A}});
  this->write(text);
  TestScan &scan = this->boot({ROM_A}, 4, nullptr, false);
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  this->log().clear();
  EXPECT_FALSE(scan.can_save());
  EXPECT_FALSE(scan.set_offset_and_save(0, 0.5f));
  EXPECT_TRUE(this->log().has(this->log().errors, "Storage unavailable: the offset is not changed"));
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.0f);
  EXPECT_TRUE(scan.set_offset_and_save(0, 0.0f));  // what it has: nothing to write
  EXPECT_EQ(this->read(), text);
}

// A file that did not load is left for a person to fix, and this boot's slots are not the table:
// no offset is written over it until a boot loads it. A forget or an assign still is.
TEST_F(Offsets, AnOffsetWaitsForASlotFileThatLoads) {
  const std::string text = R"({"version":1,"records":[{"slot":1,"address":"0x8a01)";
  this->write(text);
  TestScan &scan = this->boot({ROM_A});
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  this->log().clear();
  EXPECT_TRUE(scan.can_save());
  EXPECT_FALSE(scan.can_set_offset());
  EXPECT_FALSE(scan.set_offset_and_save(0, 0.5f));
  EXPECT_TRUE(this->log().has(this->log().errors, "The slot file did not load: the offset is not changed"));
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.0f);
  EXPECT_TRUE(scan.set_offset_and_save(0, 0.0f));  // what it has: nothing to write
  EXPECT_EQ(this->read(), text);

  ASSERT_TRUE(scan.forget_and_save(0));
  EXPECT_EQ(this->read(), slot_file({}));
  EXPECT_FALSE(scan.set_offset_and_save(0, 0.5f));  // until the reboot
  TestScan &after = this->boot({ROM_A});
  EXPECT_TRUE(after.can_set_offset());
  EXPECT_TRUE(after.set_offset_and_save(0, 0.5f));
}

TEST_F(Offsets, WithoutAMountNoOffsetCanBeSet) {
  TestScan &scan = this->boot({ROM_A}, 4, nullptr, false);
  EXPECT_FALSE(scan.can_set_offset());
  EXPECT_TRUE(this->boot_nvs({ROM_A}).can_set_offset());
}

TEST_F(Offsets, AWriteThatFailsChangesNothing) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  TestScan &scan = this->boot({ROM_A});
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  ASSERT_TRUE(scan.set_offset_and_save(0, 0.5f));
  const std::string text = this->read();
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  this->log().clear();
  EXPECT_FALSE(scan.set_offset_and_save(0, -1.0f));
  chmod(this->dir().c_str(), 0755);
  EXPECT_TRUE(this->log().has(this->log().errors, "The slot table was not written: the offset is not changed"));
  EXPECT_FALSE(this->log().has(this->log().infos, "offset"));
  EXPECT_FLOAT_EQ(scan.offset(0), 0.5f);
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.5f);
  EXPECT_EQ(this->read(), text);
}

// --- the reading it applies to ---

TEST_F(Offsets, WithoutAReadingYetNothingIsPublished) {
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_offset_and_save(0, 0.5f));
  EXPECT_FALSE(scan.sensor(0)->has_state());
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.5f);
}

TEST_F(Offsets, ASensorThatDoesNotAnswerStaysNan) {
  TestScan &scan = this->boot({ROM_A});
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  this->bus.clear_readings();
  scan.poll();
  ASSERT_TRUE(std::isnan(scan.temperature(0)));
  ASSERT_TRUE(scan.set_offset_and_save(0, 0.5f));
  EXPECT_TRUE(std::isnan(scan.temperature(0)));
  // Back, it reads with the offset.
  this->bus.set_reading(ROM_A, 21.0f);
  scan.poll();
  EXPECT_FLOAT_EQ(scan.temperature(0), 21.5f);
}

// The power-on value is dropped as read, before the offset: 85.5 with -0.5 is a reading of 85.
TEST_F(Offsets, ThePowerOnValueIsDroppedBeforeTheOffset) {
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_offset_and_save(0, -0.5f));
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  EXPECT_FLOAT_EQ(scan.temperature(0), 19.5f);
  this->bus.set_reading(ROM_A, 85.0f);
  scan.poll();
  EXPECT_FLOAT_EQ(scan.temperature(0), 19.5f);
  // The dropped value did not replace the reading the offset goes on.
  ASSERT_TRUE(scan.set_offset_and_save(0, 0.0f));
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.0f);
  ASSERT_TRUE(scan.set_offset_and_save(0, -0.5f));
  this->bus.set_reading(ROM_A, 85.5f);
  scan.poll();
  EXPECT_FLOAT_EQ(scan.temperature(0), 85.0f);
}

TEST_F(Offsets, AFreeSlotKeepsItsOffsetForTheSensorThatTakesIt) {
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_offset_and_save(1, 1.0f));
  EXPECT_EQ(scan.sensor(1), nullptr);
  EXPECT_EQ(this->read(), slot_file({{1, OFFSET_HEX_A}}, {{2, "1.0"}}));
  TestScan &after = this->boot({ROM_A, ROM_B});
  ASSERT_EQ(after.address(1), ROM_B);
  this->bus.set_reading(ROM_B, 10.0f);
  after.poll();
  EXPECT_FLOAT_EQ(after.temperature(1), 11.0f);
}

// --- the slot number keeps it ---

TEST_F(Offsets, AForgetOfOneSlotKeepsItsOffset) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_offset_and_save(0, 0.5f));
  ASSERT_TRUE(scan.forget_and_save(0));
  EXPECT_FLOAT_EQ(scan.offset(0), 0.5f);
  EXPECT_EQ(this->read(), slot_file({{2, OFFSET_HEX_B}}, {{1, "0.5"}}));
  EXPECT_FALSE(scan.can_forget(0));  // an offset alone is not a device to forget
  scan.forget(0);
  EXPECT_FLOAT_EQ(scan.offset(0), 0.5f);
  EXPECT_FLOAT_EQ(this->boot({ROM_B}).offset(0), 0.5f);
}

TEST_F(Offsets, AnAssignOrASwapLeavesTheOffsetsOnTheirSlots) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_offset_and_save(0, 0.1f));
  ASSERT_TRUE(scan.set_offset_and_save(1, -0.2f));
  ASSERT_TRUE(scan.assign_and_save(0, ROM_B));  // a swap
  ASSERT_TRUE(scan.assign_and_save(2, ROM_C));
  EXPECT_FLOAT_EQ(scan.offset(0), 0.1f);
  EXPECT_FLOAT_EQ(scan.offset(1), -0.2f);
  EXPECT_EQ(scan.offset(2), 0.0f);
  EXPECT_EQ(this->read(),
            slot_file({{1, OFFSET_HEX_B}, {2, OFFSET_HEX_A}, {3, OFFSET_HEX_C}}, {{1, "0.1"}, {2, "-0.2"}}));
  // B reads with slot 1's offset once the reboot puts it there.
  TestScan &after = this->boot({ROM_A, ROM_B});
  ASSERT_EQ(after.address(0), ROM_B);
  this->bus.set_reading(ROM_B, 30.0f);
  this->bus.set_reading(ROM_A, 20.0f);
  after.poll();
  EXPECT_FLOAT_EQ(after.temperature(0), 30.1f);
  EXPECT_FLOAT_EQ(after.temperature(1), 19.8f);
}

// Until the reboot a slot reads the device it booted with, and a new offset goes on that reading.
TEST_F(Offsets, AnOffsetSetWhileASwapWaitsGoesOnTheBootedDevicesReading) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  this->bus.set_reading(ROM_A, 20.0f);
  this->bus.set_reading(ROM_B, 30.0f);
  scan.poll();
  ASSERT_TRUE(scan.assign_and_save(0, ROM_B));
  ASSERT_TRUE(scan.slot_pending(0));
  ASSERT_TRUE(scan.set_offset_and_save(0, 0.5f));
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.5f);
  EXPECT_FLOAT_EQ(scan.temperature(1), 30.0f);
  scan.poll();
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.5f);
  EXPECT_TRUE(scan.reboot_required());
  // The reboot puts B into slot 1, which reads with that slot's offset.
  TestScan &after = this->boot({ROM_A, ROM_B});
  after.poll();
  EXPECT_FLOAT_EQ(after.temperature(0), 30.5f);
  EXPECT_FLOAT_EQ(after.temperature(1), 20.0f);
}

// --- forget all ---

TEST_F(Offsets, ForgetAllClearsTheOffsetsOfTheSlotsItEmpties) {
  TestScan &scan = this->boot({ROM_C, ROM_A}, 4, listed_c());
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  ASSERT_TRUE(scan.set_offset_and_save(1, 0.5f));
  ASSERT_TRUE(scan.set_offset_and_save(3, -1.0f));  // free
  ASSERT_FLOAT_EQ(scan.temperature(1), 20.5f);
  ASSERT_TRUE(scan.forget_and_save(-1));
  for (size_t slot = 0; slot < 4; slot++)
    EXPECT_EQ(scan.offset(slot), 0.0f) << slot;
  EXPECT_FLOAT_EQ(scan.temperature(1), 20.0f);  // at once, as a new offset would
  EXPECT_EQ(this->read(), slot_file({{1, OFFSET_HEX_C}}));
}

// A table with no device left but an offset still set has something to forget.
TEST_F(Offsets, ForgetAllWithOnlyOffsetsLeftClearsThem) {
  TestScan &scan = this->boot({ROM_A});
  ASSERT_TRUE(scan.set_offset_and_save(2, 0.4f));
  ASSERT_TRUE(scan.forget_and_save(0));
  EXPECT_FALSE(scan.can_forget(2));
  EXPECT_TRUE(scan.can_forget(-1));
  this->log().clear();
  scan.forget(-1);
  EXPECT_EQ(scan.restarts, 1);
  EXPECT_FALSE(this->log().has(this->log().infos, "Rebooting to apply"));
  EXPECT_EQ(scan.offset(2), 0.0f);
  EXPECT_FALSE(scan.can_forget(-1));
  EXPECT_EQ(this->read(), slot_file({}));
}

TEST_F(Offsets, AForgetAllWhoseWriteFailsKeepsTheOffsets) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  TestScan &scan = this->boot({ROM_A});
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  ASSERT_TRUE(scan.set_offset_and_save(0, 0.5f));
  const std::string text = this->read();
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  EXPECT_FALSE(scan.forget_and_save(-1));
  chmod(this->dir().c_str(), 0755);
  EXPECT_FLOAT_EQ(scan.offset(0), 0.5f);
  EXPECT_EQ(scan.saved_address(0), ROM_A);
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.5f);
  EXPECT_EQ(this->read(), text);
}

// --- listed slots ---

// A slot listed since the offset was stored reads its own way; the next write drops the offset.
TEST_F(Offsets, AListedSlotDropsAStoredOffset) {
  this->write(slot_file({{1, OFFSET_HEX_C}, {2, OFFSET_HEX_A}}, {{1, "0.5"}, {2, "0.2"}}));
  TestScan &scan = this->boot({ROM_C, ROM_A}, 4, listed_c());
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_FLOAT_EQ(scan.offset(1), 0.2f);
  ASSERT_TRUE(scan.set_offset_and_save(1, 0.3f));
  EXPECT_EQ(this->read(), slot_file({{1, OFFSET_HEX_C}, {2, OFFSET_HEX_A}}, {{2, "0.3"}}));
}

// The offset a listed slot has in the file is not one to forget: it is dropped at boot already.
TEST_F(Offsets, AListedSlotsStoredOffsetIsNothingToForget) {
  const std::string text = slot_file({{1, OFFSET_HEX_C}}, {{1, "0.5"}});
  this->write(text);
  TestScan &scan = this->boot({ROM_C}, 4, listed_c());
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_FALSE(scan.can_forget(-1));
  EXPECT_FALSE(scan.forget_and_save(-1));
  EXPECT_EQ(this->read(), text);
}

TEST_F(Offsets, DumpConfigListsTheOffsets) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_offset_and_save(1, -0.3f));
  ASSERT_TRUE(scan.set_offset_and_save(3, 1.0f));
  this->log().clear();
  scan.dump_config();
  EXPECT_TRUE(this->log().has(this->log().configs, "Temp 2 offset: -0.3 °C"));
  EXPECT_TRUE(this->log().has(this->log().configs, "Temp 4 offset: +1.0 °C"));
  EXPECT_FALSE(this->log().has(this->log().configs, "Temp 1 offset"));
}

// --- storage: nvs ---

static uint32_t offsets_key() { return fnv1_hash_extend(fnv1_hash("temps"), "offsets"); }

static std::string offset_prefs_path() {
  const char *prefdir = getenv("ESPHOME_PREFDIR");
  return std::string(prefdir != nullptr ? prefdir : ".prefs") + "/" + App.get_name().c_str() + ".prefs";
}

// The offsets as the host preferences file holds them after a sync, empty when they are not there.
static std::vector<int16_t> stored_offsets(size_t slots) {
  std::vector<int16_t> offsets;
  FILE *fp = fopen(offset_prefs_path().c_str(), "rb");
  if (fp == nullptr)
    return offsets;
  uint32_t key;
  uint8_t len;
  while (fread(&key, sizeof(key), 1, fp) == 1 && fread(&len, sizeof(len), 1, fp) == 1) {
    std::vector<uint8_t> data(len);
    if (fread(data.data(), 1, len, fp) != len)
      break;
    if (key == offsets_key() && len == slots * sizeof(int16_t)) {
      offsets.resize(slots);
      memcpy(offsets.data(), data.data(), len);
    }
  }
  fclose(fp);
  return offsets;
}

TEST_F(Offsets, PreferencesKeepTheOffsetsInARecordOfTheirOwn) {
  TestScan &scan = this->boot_nvs({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_offset_and_save(1, 0.7f));
  EXPECT_EQ(stored_offsets(4), (std::vector<int16_t>{0, 7, 0, 0}));  // synced before it answers
  TestScan &after = this->boot_nvs({ROM_A, ROM_B});
  EXPECT_FLOAT_EQ(after.offset(1), 0.7f);
  EXPECT_EQ(after.address(1), ROM_B);
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Offsets, PreferencesForgetAllWritesTheTableAndTheOffsets) {
  TestScan &scan = this->boot_nvs({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_offset_and_save(0, -0.4f));
  ASSERT_TRUE(scan.forget_and_save(-1));
  EXPECT_EQ(stored_offsets(4), (std::vector<int16_t>{0, 0, 0, 0}));
  TestScan &after = this->boot_nvs({ROM_B, ROM_A});
  EXPECT_EQ(after.address(0), ROM_B);
  EXPECT_EQ(after.offset(0), 0.0f);
}

// The host preferences refuse a record over 255 bytes: with 32 slots the table (256 bytes) cannot
// be written and the offsets (64 bytes) can, what a flush that fails for one record leaves on the
// device. The offsets go first, so they are cleared on flash and in memory; the table stays.
TEST_F(Offsets, PreferencesForgetAllWhoseTableFailsKeepsTheOffsetsCleared) {
  std::vector<int16_t> seeded(32, 0);
  seeded[0] = 5;
  ASSERT_TRUE(global_preferences->make_preference(seeded.size() * sizeof(int16_t), offsets_key())
                  .save(reinterpret_cast<const uint8_t *>(seeded.data()), seeded.size() * sizeof(int16_t)));
  TestScan &scan = this->boot_nvs({ROM_A}, 32);
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  ASSERT_FLOAT_EQ(scan.temperature(0), 20.5f);
  this->log().clear();
  EXPECT_FALSE(scan.forget_and_save(-1));
  EXPECT_TRUE(this->log().has(this->log().errors, "The slot table was not written: only the offsets are cleared"));
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_EQ(stored_offsets(32), std::vector<int16_t>(32, 0));
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.0f);  // published again, as on success
  EXPECT_EQ(scan.saved_address(0), ROM_A);
  EXPECT_FALSE(scan.reboot_required());
  EXPECT_TRUE(scan.can_forget(-1));  // the device is still there to forget
}

// Past 127 slots, more than YAML allows, even the offsets are over those 255 bytes: a Forget All
// whose offsets cannot be written changes nothing at all.
TEST_F(Offsets, PreferencesForgetAllWhoseOffsetsFailChangesNothing) {
  TestScan &scan = this->boot_nvs({ROM_A}, 130);
  scan.seed_offset(0, 5);
  this->log().clear();
  EXPECT_FALSE(scan.forget_and_save(-1));
  EXPECT_TRUE(this->log().has(this->log().errors, "The offsets were not written: nothing is forgotten"));
  EXPECT_FLOAT_EQ(scan.offset(0), 0.5f);
  EXPECT_EQ(scan.saved_address(0), ROM_A);
  EXPECT_FALSE(scan.reboot_required());
}

TEST_F(Offsets, AMaxSensorsChangeEmptiesTheOffsetsInPreferences) {
  ASSERT_TRUE(this->boot_nvs({ROM_A}).set_offset_and_save(0, 1.0f));
  TestScan &wider = this->boot_nvs({ROM_A}, 5);
  EXPECT_EQ(wider.offset(0), 0.0f);
  EXPECT_EQ(wider.address(0), ROM_A);
}

TEST_F(Offsets, AStoredOffsetOutOfRangeIsDropped) {
  const int16_t stored[] = {0, 51, 0, -3};
  ASSERT_TRUE(global_preferences->make_preference(sizeof(stored), offsets_key())
                  .save(reinterpret_cast<const uint8_t *>(stored), sizeof(stored)));
  TestScan &scan = this->boot_nvs({ROM_A});
  EXPECT_TRUE(this->log().has(this->log().warnings, "Slot 2: the stored offset is out of range, dropping it"));
  EXPECT_EQ(scan.offset(1), 0.0f);
  EXPECT_FLOAT_EQ(scan.offset(3), -0.3f);
}

// The host preferences refuse a record over 255 bytes, which a table of 32 slots is: the one
// preferences write a case can make fail.
TEST_F(Offsets, AnOffsetThePreferencesWillNotTakeChangesNothing) {
  TestScan &scan = this->boot_nvs({ROM_A}, 32);
  this->bus.set_reading(ROM_A, 20.0f);
  scan.poll();
  this->log().clear();
  EXPECT_FALSE(scan.set_offset_and_save(0, 0.5f));
  EXPECT_TRUE(this->log().has(this->log().errors, "Saving the slot table failed"));
  EXPECT_TRUE(this->log().has(this->log().errors, "The slot table was not written: the offset is not changed"));
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_FLOAT_EQ(scan.temperature(0), 20.0f);
}

// As for the table: a flush that fails for another record leaves this one on flash.
TEST_F(Offsets, AFailedFlushWithTheOffsetsOnFlashStillSucceeds) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes a read-only file";
  TestScan &scan = this->boot_nvs({ROM_A});
  ASSERT_TRUE(global_preferences->sync());
  ASSERT_EQ(chmod(offset_prefs_path().c_str(), 0444), 0);
  this->log().clear();
  EXPECT_TRUE(scan.set_offset_and_save(0, 0.2f));
  chmod(offset_prefs_path().c_str(), 0644);
  EXPECT_TRUE(this->log().has(this->log().warnings, "the slot table is stored all the same"));
  EXPECT_FLOAT_EQ(scan.offset(0), 0.2f);
}

// Forget All flushes twice, the offsets first: a flush that fails for another record leaves both
// on flash all the same.
TEST_F(Offsets, PreferencesForgetAllWithFailedFlushesStillSucceeds) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes a read-only file";
  TestScan &scan = this->boot_nvs({ROM_A, ROM_B});
  ASSERT_TRUE(scan.set_offset_and_save(1, 0.6f));
  ASSERT_EQ(chmod(offset_prefs_path().c_str(), 0444), 0);
  this->log().clear();
  EXPECT_TRUE(scan.forget_and_save(-1));
  chmod(offset_prefs_path().c_str(), 0644);
  EXPECT_TRUE(this->log().has(this->log().warnings, "the slot table is stored all the same"));
  EXPECT_EQ(scan.offset(1), 0.0f);
  EXPECT_EQ(scan.saved_address(0), 0u);
  EXPECT_TRUE(scan.reboot_required());
}

// --- the slot file ---

// The offsets a slot file of `slots` slots holds after parsing `text`.
static std::vector<int16_t> parsed_offsets(const std::string &text, size_t slots = 4) {
  JsonDocument doc;
  EXPECT_EQ(deserializeJson(doc, text), DeserializationError::Ok) << text;
  SlotFile file("dallas_scan_temps", slots);
  file.set_offsets(std::vector<int16_t>(slots, 9));  // a parse starts from none
  EXPECT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
  return file.offsets();
}

static std::string written_with_offsets(const std::vector<uint64_t> &table, const std::vector<int16_t> &offsets) {
  SlotFile file("dallas_scan_temps", table.size());
  file.set_table(table);
  file.set_offsets(offsets);
  JsonDocument doc;
  file.write_json(doc.to<JsonObject>(), 1);
  std::string out;
  serializeJson(doc, out);
  return out;
}

class OffsetFile : public ::testing::Test {
 protected:
  void SetUp() override { LogCapture::instance().clear(); }
  static LogCapture &log() { return LogCapture::instance(); }
  static int count(const char *needle) {
    return std::count_if(log().warnings.begin(), log().warnings.end(),
                         [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }
};

TEST_F(OffsetFile, PutsEachOffsetOnItsSlot) {
  EXPECT_EQ(parsed_offsets(R"({"records":[],"offsets":[{"slot":3,"offset":-0.3},{"slot":1,"offset":1}]})"),
            (std::vector<int16_t>{10, 0, -3, 0}));
  EXPECT_TRUE(log().warnings.empty());
}

TEST_F(OffsetFile, NoListIsNoOffsets) {
  EXPECT_EQ(parsed_offsets(R"({"records":[]})"), (std::vector<int16_t>{0, 0, 0, 0}));
  EXPECT_EQ(parsed_offsets(R"({"records":[],"offsets":null})"), (std::vector<int16_t>{0, 0, 0, 0}));
  EXPECT_TRUE(log().warnings.empty());
}

// The table is what matters: a bad list costs the offsets alone.
TEST_F(OffsetFile, AListThatIsNotAListIsIgnoredAndTheTableStays) {
  for (const char *offsets : {R"({"slot":1,"offset":0.5})", "0.5", R"("0.5")", "true"}) {
    SCOPED_TRACE(offsets);
    log().clear();
    const std::string text =
        std::string(R"({"records":[{"slot":1,"address":"0xeb01227905460228"}],"offsets":)") + offsets + "}";
    JsonDocument doc;
    ASSERT_EQ(deserializeJson(doc, text), DeserializationError::Ok);
    SlotFile file("dallas_scan_temps", 4);
    EXPECT_TRUE(file.parse_json(doc.as<JsonObject>(), 1));
    EXPECT_EQ(file.table(), (std::vector<uint64_t>{ROM_A, 0, 0, 0}));
    EXPECT_EQ(file.offsets(), (std::vector<int16_t>{0, 0, 0, 0}));
    EXPECT_TRUE(log().has(log().warnings, "'offsets' is not an array, ignoring it"));
  }
}

TEST_F(OffsetFile, AnEntryWithoutAnIntegerSlotFromOneUpIsSkipped) {
  EXPECT_EQ(parsed_offsets(R"({"records":[],"offsets":[)"
                           R"({"offset":0.5},)"
                           R"({"slot":0,"offset":0.5},)"
                           R"({"slot":-1,"offset":0.5},)"
                           R"({"slot":"2","offset":0.5},)"
                           R"({"slot":2.5,"offset":0.5},)"
                           R"({"slot":true,"offset":0.5},)"
                           R"({"slot":null,"offset":0.5},)"
                           R"(3,)"
                           R"({"slot":4,"offset":0.5}]})"),
            (std::vector<int16_t>{0, 0, 0, 5}));
  EXPECT_EQ(count("An offset without a valid slot"), 8);
}

TEST_F(OffsetFile, AnEntryPastMaxSensorsIsSkipped) {
  EXPECT_EQ(parsed_offsets(R"({"records":[],"offsets":[{"slot":5,"offset":0.5},{"slot":4,"offset":0.1}]})"),
            (std::vector<int16_t>{0, 0, 0, 1}));
  EXPECT_TRUE(log().has(log().warnings, "Slot 5's offset is past max_sensors"));
}

TEST_F(OffsetFile, AnOffsetThatIsNotANumberInRangeIsSkipped) {
  EXPECT_EQ(parsed_offsets(R"({"records":[],"offsets":[)"
                           R"({"slot":1},)"
                           R"({"slot":1,"offset":null},)"
                           R"({"slot":1,"offset":"0.5"},)"
                           R"({"slot":1,"offset":true},)"
                           R"({"slot":1,"offset":5.1},)"
                           R"({"slot":1,"offset":-5.05},)"
                           R"({"slot":1,"offset":1e300},)"
                           R"({"slot":2,"offset":-5}]})"),
            (std::vector<int16_t>{0, -50, 0, 0}));
  EXPECT_EQ(count("Slot 1: the offset is not a number within ±5.0"), 7);
}

TEST_F(OffsetFile, OfTwoEntriesForOneSlotTheLaterWins) {
  EXPECT_EQ(parsed_offsets(R"({"records":[],"offsets":[{"slot":2,"offset":0},{"slot":2,"offset":-0.2}]})"),
            (std::vector<int16_t>{0, -2, 0, 0}));
  EXPECT_TRUE(log().has(log().warnings, "Slot 2's offset is listed twice, keeping the last one"));
}

TEST_F(OffsetFile, AnOffsetIsRoundedToTheStep) {
  EXPECT_EQ(parsed_offsets(R"({"records":[],"offsets":[)"
                           R"({"slot":1,"offset":0.04},)"
                           R"({"slot":2,"offset":0.15},)"
                           R"({"slot":3,"offset":-0.25},)"
                           R"({"slot":4,"offset":5.04},)"
                           R"({"slot":5,"offset":0.35},)"
                           R"({"slot":6,"offset":-0.05}]})",
                           6),
            (std::vector<int16_t>{0, 2, -3, 50, 4, -1}));
}

TEST(OffsetFileWrite, WritesOnlyTheSlotsWithAnOffsetAsTheyReadInTenths) {
  EXPECT_EQ(written_with_offsets({ROM_A, 0, 0, 0, 0}, {0, -3, 0, 50, -1}),
            R"({"records":[{"slot":1,"address":"0xeb01227905460228"}],)"
            R"("offsets":[{"slot":2,"offset":-0.3},{"slot":4,"offset":5.0},{"slot":5,"offset":-0.1}]})");
  // None: the file is as older firmware writes it.
  EXPECT_EQ(written_with_offsets({ROM_A, 0}, {0, 0}), R"({"records":[{"slot":1,"address":"0xeb01227905460228"}]})");
}

TEST(OffsetFileWrite, WhatIsWrittenParsesBackToTheSameOffsets) {
  const std::vector<int16_t> offsets = {1, -1, 0, 49, -50, 3};
  EXPECT_EQ(parsed_offsets(written_with_offsets(std::vector<uint64_t>(6, 0), offsets), 6), offsets);
}

TEST(OffsetFileWrite, ResetClearsTheOffsets) {
  SlotFile file("dallas_scan_temps", 2);
  file.set_table({ROM_A, 0});
  file.set_offsets({5, -5});
  EXPECT_EQ(file.size(), 1u);  // records only
  file.reset();
  EXPECT_EQ(file.offsets(), (std::vector<int16_t>{0, 0}));
}

}  // namespace esphome::dallas_scan::testing
