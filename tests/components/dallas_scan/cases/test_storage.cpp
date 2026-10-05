#include "common.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>

namespace esphome::dallas_scan::testing {

static const char *const HEX_A = "0xeb01227905460228";
static const char *const HEX_B = "0x8a0122791699dd28";
static const char *const HEX_C = "0x9b01b5566e8a1f28";

// The other Dallas temperature families: DS18S20, DS1822, DS1825, DS28EA00.
static const uint64_t ROM_DS18S20 = 0x1100000000000110ULL;
static const uint64_t ROM_DS1822 = 0x2200000000000122ULL;
static const uint64_t ROM_DS1825 = 0x330000000000013bULL;
static const uint64_t ROM_DS28EA00 = 0x4400000000000142ULL;

class FileStorage : public Boots {};
class NvsStorage : public Boots {};
class Forget : public Boots {};

#define EXPECT_FORGET_REBOOTS(scan, slot) \
  do { \
    const int restarts = (scan).restarts; \
    (scan).forget(slot); \
    EXPECT_EQ((scan).restarts, restarts + 1); \
  } while (0)

// --- storage: file, at boot ---

TEST_F(FileStorage, NewDevicesTakeTheLowestFreeSlotsAndAreWrittenAtOnce) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(scan.address(1), ROM_B);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_A}, {2, HEX_B}}));
  EXPECT_EQ(this->files(), (std::vector<std::string>{"dallas_scan_temps.json"}));  // no .tmp left
}

TEST_F(FileStorage, TheNextBootReadsTheTableBack) {
  this->boot({ROM_A, ROM_B});
  // A unplugged: it keeps its slot, and a newcomer goes after both.
  TestScan &scan = this->boot({ROM_C, ROM_B});
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_NE(scan.sensor(0), nullptr);
  EXPECT_EQ(scan.address(1), ROM_B);
  EXPECT_EQ(scan.address(2), ROM_C);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_A}, {2, HEX_B}, {3, HEX_C}}));
}

TEST_F(FileStorage, WhatTakesNoSlotIsNotWritten) {
  // Not a thermometer, and no slot left for the third one.
  TestScan &scan = this->boot({ROM_SERIAL, ROM_A, ROM_B, ROM_C}, 2);
  EXPECT_TRUE(this->log().has(this->log().warnings, "Not a temperature sensor, skipping 0x4e00001234567801"));
  EXPECT_TRUE(this->log().has(this->log().warnings, "No free slot for 0x9b01b5566e8a1f28"));
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(scan.address(1), ROM_B);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_A}, {2, HEX_B}}));
}

TEST_F(FileStorage, AHandWrittenFilePutsEachDeviceInItsSlot) {
  this->write(slot_file({{3, HEX_A}, {1, HEX_B}}));
  TestScan &scan = this->boot({ROM_A, ROM_B, ROM_C});
  EXPECT_EQ(scan.address(0), ROM_B);
  EXPECT_EQ(scan.address(1), ROM_C);  // the lowest free one
  EXPECT_EQ(scan.address(2), ROM_A);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_B}, {2, HEX_C}, {3, HEX_A}}));
}

TEST_F(FileStorage, ABootThatChangesNothingDoesNotWrite) {
  const std::string text = "{\n  \"records\": [\n    {\"address\": \"0XEB01227905460228\", \"slot\": 2}\n  ]\n}\n";
  this->write(text);
  EXPECT_EQ(this->boot({ROM_A}).address(1), ROM_A);
  EXPECT_EQ(this->read(), text);
  // Nor when the device is unplugged: it keeps the slot.
  EXPECT_EQ(this->boot({}).address(1), ROM_A);
  EXPECT_EQ(this->read(), text);
}

TEST_F(FileStorage, OnlyDallasTemperatureFamiliesStayInTheTable) {
  this->write(slot_file({{1, "0x4e00001234567801"},
                         {2, HEX_A},
                         {3, "0x1100000000000110"},
                         {4, "0x2200000000000122"},
                         {5, "0x330000000000013b"},
                         {6, "0x4400000000000142"}}));
  TestScan &scan = this->boot({ROM_A, ROM_B}, 6);
  EXPECT_TRUE(this->log().has(this->log().warnings, "dropping 0x4e00001234567801 from the table"));
  // The serial number's slot was free, so the newcomer takes it, and the write leaves it out.
  EXPECT_EQ(scan.address(0), ROM_B);
  EXPECT_EQ(scan.address(1), ROM_A);
  EXPECT_EQ(scan.address(2), ROM_DS18S20);
  EXPECT_EQ(scan.address(3), ROM_DS1822);
  EXPECT_EQ(scan.address(4), ROM_DS1825);
  EXPECT_EQ(scan.address(5), ROM_DS28EA00);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_B},
                                     {2, HEX_A},
                                     {3, "0x1100000000000110"},
                                     {4, "0x2200000000000122"},
                                     {5, "0x330000000000013b"},
                                     {6, "0x4400000000000142"}}));
}

TEST_F(FileStorage, ADroppedAddressAloneDoesNotRewriteTheFile) {
  const std::string text = slot_file({{1, "0x4e00001234567801"}, {2, HEX_A}});
  this->write(text);
  TestScan &scan = this->boot({ROM_A});
  EXPECT_EQ(scan.address(0), 0u);
  EXPECT_EQ(scan.address(1), ROM_A);
  EXPECT_EQ(this->read(), text);
}

TEST_F(FileStorage, ARecordPastMaxSensorsWaitsInTheFileUntilAWrite) {
  const std::string text = slot_file({{5, HEX_C}, {1, HEX_B}});
  this->write(text);
  EXPECT_EQ(this->boot({ROM_B}).address(0), ROM_B);
  EXPECT_EQ(this->read(), text);
  // A newcomer rewrites the table, and only what fits in max_sensors is in it.
  TestScan &scan = this->boot({ROM_B, ROM_A});
  EXPECT_EQ(scan.address(1), ROM_A);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_B}, {2, HEX_A}}));
}

TEST_F(FileStorage, AListedSensorTakesItsAddressOutOfAnotherSlot) {
  this->write(slot_file({{2, HEX_A}}));
  TestScan &scan = this->boot({ROM_A}, 4, [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_A);
  });
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(scan.address(1), 0u);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_A}}));
}

TEST_F(FileStorage, AFileThatDidNotLoadIsNotWrittenOver) {
  struct Case {
    const char *name;
    std::string text;
  };
  const std::vector<Case> cases = {
      {"corrupt", R"({"version":1,"records":[{"slot":1,"address":"0x8a01)"},
      {"empty", ""},
      {"not an object", R"([{"slot":1,"address":"0x8a0122791699dd28"}])"},
      {"records not a list", R"({"version":1,"records":{"slot":1,"address":"0x8a0122791699dd28"}})"},
      {"too large", std::string(70000, ' ')},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.name);
    this->log().clear();
    this->write(c.text);
    // The devices take slots in bus order for this boot only.
    TestScan &scan = this->boot({ROM_A});
    EXPECT_EQ(scan.address(0), ROM_A);
    EXPECT_TRUE(this->log().has(this->log().warnings, "dallas_scan_temps.json did not load"));
    EXPECT_EQ(this->read(), c.text);
    EXPECT_EQ(this->files(), (std::vector<std::string>{"dallas_scan_temps.json"}));
  }
}

// There but unopenable is not an empty start either.
TEST_F(FileStorage, AFileThatWillNotOpenIsNotWrittenOver) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root reads a file whatever its mode";
  const std::string text = slot_file({{1, HEX_B}});
  this->write(text);
  ASSERT_EQ(chmod(this->file().c_str(), 0), 0);
  TestScan &scan = this->boot({ROM_A, ROM_B});
  chmod(this->file().c_str(), 0644);
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(this->read(), text);
}

TEST_F(FileStorage, AWriteThatFailsLeavesTheSlotsForThisBootOnly) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  ASSERT_EQ(mkdir(this->dir().c_str(), 0555), 0);
  TestScan &scan = this->boot({ROM_A});
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_TRUE(this->log().has(this->log().errors, "Failed to open"));
  EXPECT_TRUE(this->files().empty());
  ASSERT_EQ(chmod(this->dir().c_str(), 0755), 0);
  EXPECT_EQ(this->boot({ROM_B, ROM_A}).address(0), ROM_B);
}

TEST_F(FileStorage, WithoutAMountTheSlotsLastOneBootAndNothingIsForgotten) {
  const std::string text = slot_file({{1, HEX_B}});
  this->write(text);
  TestScan &scan = this->boot({ROM_A, ROM_B}, 4, nullptr, false);
  // Not read: the keeper failed, so the devices go in bus order.
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(scan.address(1), ROM_B);
  EXPECT_TRUE(this->log().has(this->log().errors, "Storage unavailable: the slots last until the next reboot"));
  EXPECT_EQ(this->read(), text);

  this->log().clear();
  scan.forget(0);
  scan.assign(2, ROM_C);
  EXPECT_EQ(scan.restarts, 0);
  EXPECT_FALSE(scan.can_save());
  EXPECT_TRUE(this->log().has(this->log().errors, "Storage unavailable: nothing is forgotten"));
  EXPECT_TRUE(this->log().has(this->log().errors, "Storage unavailable: nothing is assigned"));
  EXPECT_EQ(scan.saved_address(0), ROM_A);
  EXPECT_EQ(scan.saved_address(2), 0u);
  EXPECT_FALSE(scan.reboot_required());
  EXPECT_EQ(this->read(), text);
}

TEST_F(FileStorage, AForgetThatWouldChangeNothingNeitherWritesNorReboots) {
  auto listing = [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_A);
  };
  TestScan &scan = this->boot({ROM_A}, 4, listing);
  const std::string text = this->read();
  ASSERT_EQ(text, slot_file({{1, HEX_A}}));
  scan.forget(0);   // listed
  scan.forget(2);   // free
  scan.forget(-1);  // nothing but the listed one
  EXPECT_EQ(scan.restarts, 0);
  EXPECT_TRUE(this->log().has(this->log().warnings, "Nothing to forget"));
  EXPECT_EQ(this->read(), text);
}

// A reboot after a failed write would bring the slot back without a word.
TEST_F(FileStorage, AForgetWhoseWriteFailsKeepsTheSlotAndDoesNotReboot) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  this->write(slot_file({{1, HEX_A}, {2, HEX_B}}));
  TestScan &scan = this->boot({ROM_A, ROM_B});
  const std::string text = this->read();
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  scan.forget(0);
  chmod(this->dir().c_str(), 0755);
  EXPECT_EQ(scan.restarts, 0);
  EXPECT_EQ(scan.saved_address(0), ROM_A);
  EXPECT_FALSE(scan.reboot_required());
  EXPECT_TRUE(this->log().has(this->log().errors, "nothing is forgotten"));
  EXPECT_EQ(this->read(), text);
}

TEST_F(FileStorage, AnAssignWhoseWriteFailsKeepsTheTableAndDoesNotReboot) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  TestScan &scan = this->boot({ROM_A, ROM_B});
  const std::string text = this->read();
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  this->log().clear();
  scan.assign(0, ROM_B);  // a swap
  chmod(this->dir().c_str(), 0755);
  EXPECT_EQ(scan.restarts, 0);
  EXPECT_FALSE(scan.reboot_required());
  // Nothing claims the device moved.
  EXPECT_FALSE(this->log().has(this->log().infos, "takes slot"));
  EXPECT_EQ(scan.saved_address(0), ROM_A);
  EXPECT_EQ(scan.saved_address(1), ROM_B);
  EXPECT_TRUE(this->log().has(this->log().errors, "nothing is assigned"));
  EXPECT_EQ(this->read(), text);
}

// The table in memory follows the file: a failed write in a stack undoes itself, not the edits
// before it, even one that would have put the table back as booted.
TEST_F(FileStorage, AFailedWriteInAStackUndoesOnlyItself) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.assign_and_save(0, ROM_B));
  const std::string text = this->read();
  ASSERT_EQ(text, slot_file({{1, HEX_B}, {2, HEX_A}}));
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  EXPECT_FALSE(scan.forget_and_save(0));
  EXPECT_FALSE(scan.assign_and_save(0, ROM_A));  // the swap back
  chmod(this->dir().c_str(), 0755);
  EXPECT_EQ(scan.saved_address(0), ROM_B);
  EXPECT_EQ(scan.saved_address(1), ROM_A);
  EXPECT_TRUE(scan.reboot_required());
  EXPECT_EQ(this->read(), text);
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(FileStorage, AnAssignIsWrittenToTheFile) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  scan.assign(0, ROM_B);
  EXPECT_EQ(scan.restarts, 1);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_B}, {2, HEX_A}}));
}

// The halves the web dashboard calls: the change is saved and waits for a reboot the user picks.
TEST_F(FileStorage, SavingWithoutTheRebootLeavesTheRebootToTheCaller) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  EXPECT_FALSE(scan.forget_and_save(2));  // free
  EXPECT_FALSE(scan.reboot_required());
  EXPECT_TRUE(scan.forget_and_save(0));
  EXPECT_TRUE(scan.reboot_required());
  EXPECT_EQ(scan.restarts, 0);
  EXPECT_EQ(this->read(), slot_file({{2, HEX_B}}));

  TestScan &next = this->boot({ROM_B});
  EXPECT_FALSE(next.reboot_required());
  EXPECT_FALSE(next.assign_and_save(1, ROM_B));  // there already
  EXPECT_TRUE(next.assign_and_save(2, ROM_C));
  EXPECT_TRUE(next.reboot_required());
  EXPECT_TRUE(this->log().has(this->log().infos, "0x9b01b5566e8a1f28 takes slot 3 after a reboot"));
  EXPECT_EQ(next.restarts, 0);
  EXPECT_EQ(this->read(), slot_file({{2, HEX_B}, {3, HEX_C}}));
}

TEST_F(FileStorage, SeveralEditsStackUntilOneRebootBindsThemAll) {
  TestScan &scan = this->boot({ROM_A, ROM_B, ROM_C});
  ASSERT_TRUE(scan.forget_and_save(2));
  EXPECT_EQ(this->read(), slot_file({{1, HEX_A}, {2, HEX_B}}));
  ASSERT_TRUE(scan.assign_and_save(0, ROM_B));  // a swap
  EXPECT_EQ(this->read(), slot_file({{1, HEX_B}, {2, HEX_A}}));
  ASSERT_TRUE(scan.assign_and_save(3, ROM_C));  // the forgotten device, past the bound slots
  EXPECT_EQ(this->read(), slot_file({{1, HEX_B}, {2, HEX_A}, {4, HEX_C}}));
  EXPECT_TRUE(scan.reboot_required());
  EXPECT_EQ(scan.restarts, 0);

  TestScan &after = this->boot({ROM_A, ROM_B, ROM_C});
  EXPECT_EQ(after.address(0), ROM_B);
  EXPECT_EQ(after.address(1), ROM_A);
  EXPECT_EQ(after.address(2), 0u);
  EXPECT_EQ(after.address(3), ROM_C);
  EXPECT_FALSE(after.reboot_required());
  EXPECT_EQ(this->read(), slot_file({{1, HEX_B}, {2, HEX_A}, {4, HEX_C}}));
}

// Until the reboot each sensor reads the device it booted with: the harness bus answers all
// ones, so every read fails its checksum and names the ROM it went to.
TEST_F(FileStorage, AnEditedTableKeepsTheReadsOnTheBootDevices) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.assign_and_save(0, ROM_B));  // {B, A}
  ASSERT_TRUE(scan.forget_and_save(1));         // {B, -}
  this->log().clear();
  scan.update();
  std::this_thread::sleep_for(std::chrono::milliseconds(800));  // the 12-bit conversion
  for (int pass = 0; pass < 8; pass++)
    App.scheduler.call(millis());
  EXPECT_TRUE(this->log().has(this->log().warnings, "Temp 1: 0xeb01227905460228 does not answer"));
  EXPECT_TRUE(this->log().has(this->log().warnings, "Temp 2: 0x8a0122791699dd28 does not answer"));
  EXPECT_FALSE(this->log().has(this->log().warnings, "Temp 1: 0x8a0122791699dd28"));
  EXPECT_FALSE(this->log().has(this->log().warnings, "0x0000000000000000"));
  EXPECT_TRUE(scan.status_has_warning());
}

TEST_F(FileStorage, DumpConfigNamesTheFile) {
  TestScan &scan = this->boot({ROM_A});
  this->log().clear();
  scan.dump_config();
  EXPECT_TRUE(this->log().has(this->log().configs, "Slot file: config/dallas_scan_temps.json"));
  EXPECT_TRUE(this->log().has(this->log().configs, "Temp 1: 0xeb01227905460228 (DS18B20)"));
}

TEST_F(FileStorage, DumpConfigNamesTheBootDevicesAfterAnEdit) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.assign_and_save(0, ROM_B));
  this->log().clear();
  scan.dump_config();
  EXPECT_TRUE(this->log().has(this->log().configs, "Temp 1: 0xeb01227905460228 (DS18B20)"));
  EXPECT_TRUE(this->log().has(this->log().configs, "Temp 2: 0x8a0122791699dd28 (DS18B20)"));
}

TEST_F(FileStorage, TheTwoStoragesShareNothing) {
  this->boot({ROM_A});
  EXPECT_EQ(this->read(), slot_file({{1, HEX_A}}));
  TestScan &nvs = this->boot_nvs({ROM_B, ROM_A});
  EXPECT_EQ(nvs.address(0), ROM_B);
  EXPECT_EQ(nvs.address(1), ROM_A);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_A}}));
}

// --- storage: file, forget ---

TEST_F(Forget, ForgetWritesTheSlotOutOfTheFileAndReboots) {
  this->write(slot_file({{1, HEX_A}, {2, HEX_B}}));
  TestScan &before = this->boot({ROM_B});  // A unplugged
  EXPECT_FORGET_REBOOTS(before, 0);
  EXPECT_EQ(this->read(), slot_file({{2, HEX_B}}));
  // After the reboot the slot is free, and the next newcomer takes it.
  TestScan &after = this->boot({ROM_B, ROM_C});
  EXPECT_EQ(after.address(0), ROM_C);
  EXPECT_EQ(after.address(1), ROM_B);
}

// The keeper made the folder at boot; a forget makes it again if it has gone since.
TEST_F(Forget, ForgetRecreatesAFolderRemovedSinceBoot) {
  this->write(slot_file({{1, HEX_A}, {2, HEX_B}}));
  TestScan &before = this->boot({ROM_B});
  ASSERT_EQ(remove(this->file().c_str()), 0);
  ASSERT_EQ(rmdir(this->dir().c_str()), 0);
  EXPECT_FORGET_REBOOTS(before, 0);
  EXPECT_EQ(this->read(), slot_file({{2, HEX_B}}));
}

TEST_F(Forget, ForgetAllLeavesOnlyTheListedSlot) {
  auto listing = [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_C);
  };
  this->write(slot_file({{1, HEX_C}, {2, HEX_B}, {3, HEX_A}}));
  TestScan &before = this->boot({ROM_C, ROM_A, ROM_B}, 4, listing);
  EXPECT_FORGET_REBOOTS(before, -1);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_C}}));
  TestScan &after = this->boot({ROM_C, ROM_A, ROM_B}, 4, listing);
  EXPECT_EQ(after.address(1), ROM_A);
  EXPECT_EQ(after.address(2), ROM_B);
}

TEST_F(Forget, OnlyAForgetOrAnAssignWritesOverAFileThatDidNotLoad) {
  const std::string broken = R"({"records":[{"slot":1,"address":"0x8a01)";
  this->write(broken);
  TestScan &forgetting = this->boot({ROM_A, ROM_B});
  ASSERT_EQ(this->read(), broken);
  EXPECT_FORGET_REBOOTS(forgetting, 0);
  EXPECT_EQ(this->read(), slot_file({{2, HEX_B}}));

  this->write(broken);
  TestScan &assigning = this->boot({ROM_A, ROM_B});
  ASSERT_EQ(this->read(), broken);
  assigning.assign(0, ROM_C);
  EXPECT_EQ(assigning.restarts, 1);
  EXPECT_EQ(this->read(), slot_file({{1, HEX_C}, {2, HEX_B}}));
}

// The panel's Confirm on a slot the dashboard forgot already applies what waits.
TEST_F(Forget, ForgetRebootsForWhatTheSavedTableForgotAlready) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_TRUE(scan.forget_and_save(0));
  const std::string text = this->read();
  this->log().clear();
  EXPECT_FORGET_REBOOTS(scan, 0);
  EXPECT_TRUE(this->log().has(this->log().infos, "Forgotten already; rebooting"));
  EXPECT_FALSE(this->log().has(this->log().warnings, "Nothing to forget"));
  EXPECT_EQ(this->read(), text);
  ASSERT_TRUE(scan.forget_and_save(-1));
  EXPECT_FORGET_REBOOTS(scan, -1);
  EXPECT_EQ(this->read(), slot_file({}));
}

// Only an empty slot in the saved table is forgotten already: a listed slot, one past the table,
// a write that fails and a store that cannot be written do not reboot, whatever waits.
TEST_F(Forget, WhileAChangeWaitsWhatForgetsNothingStillDoesNotReboot) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  auto listing = [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_C);
  };
  TestScan &scan = this->boot({ROM_C, ROM_A, ROM_B}, 4, listing);
  ASSERT_TRUE(scan.forget_and_save(1));
  ASSERT_TRUE(scan.reboot_required());
  scan.forget(0);  // listed
  scan.forget(4);  // past the table
  ASSERT_EQ(chmod(this->dir().c_str(), 0555), 0);
  scan.forget(2);  // the write fails
  chmod(this->dir().c_str(), 0755);
  EXPECT_EQ(scan.saved_address(2), ROM_B);
  this->keepers.back()->mark_failed();
  scan.forget(1);  // the store went away
  EXPECT_EQ(scan.restarts, 0);
}

// --- storage: nvs ---

static std::string prefs_path() {
  const char *prefdir = getenv("ESPHOME_PREFDIR");
  return std::string(prefdir != nullptr ? prefdir : ".prefs") + "/" + App.get_name().c_str() + ".prefs";
}

// The table as the host preferences file holds it after a sync, empty when it is not there.
static std::vector<uint64_t> stored_table(size_t slots) {
  std::vector<uint64_t> table;
  FILE *fp = fopen(prefs_path().c_str(), "rb");
  if (fp == nullptr)
    return table;
  uint32_t key;
  uint8_t len;
  while (fread(&key, sizeof(key), 1, fp) == 1 && fread(&len, sizeof(len), 1, fp) == 1) {
    std::vector<uint8_t> data(len);
    if (fread(data.data(), 1, len, fp) != len)
      break;
    if (key == fnv1_hash("temps") && len == slots * sizeof(uint64_t)) {
      table.resize(slots);
      memcpy(table.data(), data.data(), len);
    }
  }
  fclose(fp);
  return table;
}

TEST_F(NvsStorage, TheTableComesBackFromPreferencesAndNoFileIsWritten) {
  this->boot_nvs({ROM_A, ROM_B});
  TestScan &scan = this->boot_nvs({ROM_C, ROM_B});
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(scan.address(1), ROM_B);
  EXPECT_EQ(scan.address(2), ROM_C);
  EXPECT_TRUE(this->files().empty());
}

// Each edit is on flash before it answers, and the next boot binds them all.
TEST_F(NvsStorage, EveryEditIsSyncedAndTheyStackUntilOneReboot) {
  TestScan &scan = this->boot_nvs({ROM_A, ROM_B});
  ASSERT_TRUE(scan.forget_and_save(0));
  EXPECT_EQ(stored_table(4), (std::vector<uint64_t>{0, ROM_B, 0, 0}));
  ASSERT_TRUE(scan.assign_and_save(2, ROM_A));
  EXPECT_EQ(stored_table(4), (std::vector<uint64_t>{0, ROM_B, ROM_A, 0}));
  EXPECT_TRUE(scan.reboot_required());
  ASSERT_TRUE(scan.assign_and_save(0, ROM_C));
  EXPECT_EQ(stored_table(4), (std::vector<uint64_t>{ROM_C, ROM_B, ROM_A, 0}));
  EXPECT_EQ(scan.restarts, 0);
  EXPECT_EQ(scan.address(0), ROM_A);

  TestScan &after = this->boot_nvs({ROM_A, ROM_B});
  EXPECT_EQ(after.address(0), ROM_C);
  EXPECT_EQ(after.address(1), ROM_B);
  EXPECT_EQ(after.address(2), ROM_A);
  EXPECT_FALSE(after.reboot_required());
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Forget, ForgetInPreferencesSyncsTheTableAndReboots) {
  TestScan &scan = this->boot_nvs({ROM_A, ROM_B});
  EXPECT_FORGET_REBOOTS(scan, 0);
  EXPECT_EQ(stored_table(4), (std::vector<uint64_t>{0, ROM_B, 0, 0}));
  EXPECT_TRUE(this->files().empty());
}

// The flush reports for every record at once. On the host a record lives in memory from save()
// on, so a flush that cannot write the file is the case where another record failed and this
// one is on flash: the read-back finds the new table and the forget goes ahead.
TEST_F(Forget, AFailedFlushWithTheTableOnFlashStillReboots) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes a read-only file";
  TestScan &scan = this->boot_nvs({ROM_A, ROM_B});
  ASSERT_TRUE(global_preferences->sync());
  ASSERT_EQ(chmod(prefs_path().c_str(), 0444), 0);
  EXPECT_FORGET_REBOOTS(scan, 0);
  chmod(prefs_path().c_str(), 0644);
}

}  // namespace esphome::dallas_scan::testing
