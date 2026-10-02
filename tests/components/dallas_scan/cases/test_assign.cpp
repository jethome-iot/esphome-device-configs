#include "common.h"

namespace esphome::dallas_scan::testing {

// --- what a slot can hold ---

TEST(Address, AThermometerRomWithItsCrcIsValid) {
  for (uint64_t rom : {ROM_A, ROM_B, ROM_C})
    EXPECT_TRUE(DallasScan::valid_address(rom)) << std::hex << rom;
}

TEST(Address, AnotherFamilyOrABrokenCrcIsNot) {
  EXPECT_FALSE(DallasScan::valid_address(ROM_SERIAL));            // CRC fine, not a thermometer
  EXPECT_FALSE(DallasScan::valid_address(ROM_A ^ (1ULL << 63)));  // one bit of the CRC off
  EXPECT_FALSE(DallasScan::valid_address(ROM_A ^ (1ULL << 20)));  // one bit of the serial off
  EXPECT_FALSE(DallasScan::valid_address(0));
}

// --- what assign may touch ---

TEST_F(Slots, AssignRefusesWhatItCannotOrNeedNotDo) {
  TestScan &scan = this->boot({ROM_A, ROM_B}, 4, [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_C);
  });
  ASSERT_EQ(scan.address(1), ROM_A);
  EXPECT_EQ(scan.check_assign(4, ROM_B), AssignCheck::BAD_SLOT);
  EXPECT_EQ(scan.check_assign(2, ROM_SERIAL), AssignCheck::BAD_ADDRESS);
  EXPECT_EQ(scan.check_assign(2, ROM_B ^ (1ULL << 63)), AssignCheck::BAD_ADDRESS);
  EXPECT_EQ(scan.check_assign(0, ROM_A), AssignCheck::LISTED_SLOT);
  EXPECT_EQ(scan.check_assign(3, ROM_C), AssignCheck::LISTED_ADDRESS);
  EXPECT_EQ(scan.check_assign(1, ROM_A), AssignCheck::UNCHANGED);
  EXPECT_EQ(scan.check_assign(3, ROM_A), AssignCheck::OK);
  // None of those reboots.
  scan.assign(0, ROM_A);
  scan.assign(3, ROM_C);
  scan.assign(1, ROM_A);
  EXPECT_EQ(scan.restarts, 0);
}

// --- assign ---

TEST_F(Slots, AssignMovesADeviceToAFreeSlot) {
  TestScan &before = this->boot({ROM_A, ROM_B});
  before.assign(3, ROM_A);
  EXPECT_EQ(before.restarts, 1);
  // Slot 1 is free now, and stays free: A is in the table, so the scan leaves it where it is.
  TestScan &after = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(after.address(0), 0u);
  EXPECT_EQ(after.sensor(0), nullptr);
  EXPECT_EQ(after.address(1), ROM_B);
  EXPECT_EQ(after.address(3), ROM_A);
  EXPECT_EQ(after.used_slots(), 4u);
}

TEST_F(Slots, AssignSwapsTwoBoundSlots) {
  this->boot({ROM_A, ROM_B}).assign(0, ROM_B);
  TestScan &after = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(after.address(0), ROM_B);
  EXPECT_EQ(after.address(1), ROM_A);
  EXPECT_EQ(after.slot_name(0), "Temp 1");
}

TEST_F(Slots, ANewAddressTakesTheSlotAndItsDeviceLosesIt) {
  this->boot({ROM_A, ROM_B}).assign(1, ROM_C);
  // B is still on the bus, out of the table: it takes the lowest free slot.
  TestScan &after = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(after.address(0), ROM_A);
  EXPECT_EQ(after.address(1), ROM_C);
  EXPECT_NE(after.sensor(1), nullptr);  // not on the bus yet, but the slot is its
  EXPECT_EQ(after.address(2), ROM_B);
}

TEST_F(Slots, AnAddressAssignedAheadKeepsItsSlotWhenTheDeviceArrives) {
  // What the panel cannot do: give a sensor its number before it is plugged in.
  this->boot({ROM_A}).assign(2, ROM_C);
  TestScan &plugged = this->boot({ROM_C, ROM_A, ROM_B});
  EXPECT_EQ(plugged.address(0), ROM_A);
  EXPECT_EQ(plugged.address(1), ROM_B);
  EXPECT_EQ(plugged.address(2), ROM_C);
}

TEST_F(Slots, AssignLeavesTheListedSlotsAsTheyAre) {
  auto listing = [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_C);
  };
  this->boot({ROM_C, ROM_A, ROM_B}, 4, listing).assign(3, ROM_A);
  TestScan &after = this->boot({ROM_C, ROM_A, ROM_B}, 4, listing);
  EXPECT_EQ(after.address(0), ROM_C);
  EXPECT_EQ(after.address(1), 0u);
  EXPECT_EQ(after.address(2), ROM_B);
  EXPECT_EQ(after.address(3), ROM_A);
}

}  // namespace esphome::dallas_scan::testing
