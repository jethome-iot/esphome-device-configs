#include "common.h"

namespace esphome::dallas_scan::testing {

// --- binding at boot ---

TEST_F(Slots, EachDeviceTakesTheLowestFreeSlotInBusOrder) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(scan.address(1), ROM_B);
  EXPECT_EQ(scan.address(2), 0u);
  EXPECT_EQ(scan.used_slots(), 2u);
  EXPECT_EQ(scan.slot_name(0), "Temp 1");
  EXPECT_EQ(scan.slot_name(1), "Temp 2");
  ASSERT_EQ(scan.sensors().size(), 2u);
  EXPECT_EQ(scan.sensors()[0], scan.sensor(0));
}

TEST_F(Slots, ADeviceKeepsItsSlotAcrossBoots) {
  this->boot({ROM_A, ROM_B});
  // A unplugged: its slot keeps its address and its sensor, which reads nothing.
  TestScan &without_a = this->boot({ROM_B});
  EXPECT_EQ(without_a.address(0), ROM_A);
  EXPECT_NE(without_a.sensor(0), nullptr);
  EXPECT_EQ(without_a.address(1), ROM_B);
  // A newcomer goes after both, even with A gone.
  TestScan &with_c = this->boot({ROM_C, ROM_B});
  EXPECT_EQ(with_c.address(0), ROM_A);
  EXPECT_EQ(with_c.address(1), ROM_B);
  EXPECT_EQ(with_c.address(2), ROM_C);
}

TEST_F(Slots, ADeviceThatIsNotAThermometerTakesNoSlot) {
  TestScan &scan = this->boot({ROM_SERIAL, ROM_A});
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(scan.used_slots(), 1u);
}

TEST_F(Slots, ADeviceWithNoSlotLeftIsSkipped) {
  TestScan &scan = this->boot({ROM_A, ROM_B, ROM_C}, 2);
  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(scan.address(1), ROM_B);
  EXPECT_EQ(scan.used_slots(), 2u);
}

TEST_F(Slots, AListedSensorTakesTheFirstSlotAndTheScanFillsTheRest) {
  TestScan &scan = this->boot({ROM_A}, 4, [](TestScan &s) { s.set_sensor(0, &boiler()); });
  EXPECT_TRUE(scan.pinned(0));
  EXPECT_EQ(scan.sensor(0), &boiler());
  EXPECT_EQ(scan.slot_name(0), "Boiler");
  // Not a 1-Wire sensor, so no address of its own.
  EXPECT_EQ(scan.address(0), 0u);
  EXPECT_FALSE(scan.pinned(1));
  EXPECT_EQ(scan.address(1), ROM_A);
}

TEST_F(Slots, AListedOneWireSensorKeepsItsDeviceOutOfTheScan) {
  TestScan &scan = this->boot({ROM_A, ROM_B}, 4, [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_B);
  });
  EXPECT_EQ(scan.address(0), ROM_B);
  EXPECT_EQ(scan.address(1), ROM_A);
  EXPECT_EQ(scan.address(2), 0u);
}

// --- what forget may touch ---

TEST_F(Slots, ASlotHoldingADeviceCanBeForgotten) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  EXPECT_TRUE(scan.can_forget(0));
  EXPECT_TRUE(scan.can_forget(1));
  EXPECT_TRUE(scan.can_forget(-1));
}

TEST_F(Slots, AnUnpluggedDeviceCanBeForgotten) {
  // The case the feature is for: the sensor is gone and its slot is still held.
  this->boot({ROM_A, ROM_B});
  TestScan &scan = this->boot({ROM_B});
  EXPECT_TRUE(scan.can_forget(0));
}

TEST_F(Slots, AFreeSlotOrOnePastTheTableCannotBeForgotten) {
  TestScan &scan = this->boot({ROM_A});
  EXPECT_FALSE(scan.can_forget(1));
  EXPECT_FALSE(scan.can_forget(4));
  EXPECT_FALSE(scan.can_forget(1000));
}

TEST_F(Slots, AListedSlotCannotBeForgotten) {
  TestScan &scan = this->boot({ROM_A}, 4, [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_A);
  });
  EXPECT_FALSE(scan.can_forget(0));
  // Listed slots are all there is, so forgetting every slot would change nothing either.
  EXPECT_FALSE(scan.can_forget(-1));
}

TEST_F(Slots, AnEmptyTableHasNothingToForget) {
  TestScan &scan = this->boot({});
  EXPECT_FALSE(scan.can_forget(-1));
  EXPECT_EQ(scan.used_slots(), 0u);
}

// --- forget ---

TEST_F(Slots, ForgetEmptiesTheSlotAndReboots) {
  this->boot({ROM_A, ROM_B});
  TestScan &before = this->boot({ROM_B});  // A unplugged
  before.forget(0);
  EXPECT_EQ(before.restarts, 1);
  // The slot is free after the reboot; B keeps its number, and the free slot keeps its row.
  TestScan &after = this->boot({ROM_B});
  EXPECT_EQ(after.address(0), 0u);
  EXPECT_EQ(after.sensor(0), nullptr);
  EXPECT_EQ(after.slot_name(0), "Temp 1");
  EXPECT_EQ(after.address(1), ROM_B);
  EXPECT_EQ(after.used_slots(), 2u);
  EXPECT_FALSE(after.can_forget(0));
  // The next newcomer takes it.
  TestScan &with_c = this->boot({ROM_B, ROM_C});
  EXPECT_EQ(with_c.address(0), ROM_C);
}

TEST_F(Slots, AForgottenDeviceStillOnTheBusComesBackToTheLowestFreeSlot) {
  this->boot({ROM_A});
  this->boot({ROM_B, ROM_A}).forget(0);
  TestScan &after = this->boot({ROM_B, ROM_A});
  EXPECT_EQ(after.address(0), ROM_A);
  EXPECT_EQ(after.address(1), ROM_B);
}

TEST_F(Slots, ForgetAllNumbersTheDevicesAgainInBusOrder) {
  this->boot({ROM_B});
  TestScan &before = this->boot({ROM_A, ROM_B});
  ASSERT_EQ(before.address(0), ROM_B);
  ASSERT_EQ(before.address(1), ROM_A);
  before.forget(-1);
  EXPECT_EQ(before.restarts, 1);
  TestScan &after = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(after.address(0), ROM_A);
  EXPECT_EQ(after.address(1), ROM_B);
}

TEST_F(Slots, ForgetAllLeavesTheListedSlots) {
  auto listing = [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_C);
  };
  this->boot({ROM_C, ROM_B}, 4, listing);
  TestScan &before = this->boot({ROM_C, ROM_A, ROM_B}, 4, listing);
  ASSERT_EQ(before.address(1), ROM_B);
  ASSERT_EQ(before.address(2), ROM_A);
  before.forget(-1);
  EXPECT_EQ(before.restarts, 1);
  TestScan &after = this->boot({ROM_C, ROM_A, ROM_B}, 4, listing);
  EXPECT_EQ(after.address(0), ROM_C);
  EXPECT_EQ(after.address(1), ROM_A);
  EXPECT_EQ(after.address(2), ROM_B);
}

TEST_F(Slots, ForgetThatWouldChangeNothingDoesNotReboot) {
  TestScan &scan = this->boot({ROM_A}, 4, [](TestScan &s) {
    s.set_sensor(0, &boiler());
    s.pin(0, ROM_A);
  });
  scan.forget(0);   // listed
  scan.forget(2);   // free
  scan.forget(-1);  // nothing but the listed one
  EXPECT_EQ(scan.restarts, 0);
  EXPECT_EQ(this->boot({ROM_A}, 4,
                       [](TestScan &s) {
                         s.set_sensor(0, &boiler());
                         s.pin(0, ROM_A);
                       })
                .address(0),
            ROM_A);
}

// --- edits that wait for a reboot ---

// What this boot's sensors are stays as booted; the saved table and its pending slots show the
// edits, and each edit is checked against the ones before it.
TEST_F(Slots, TheBootViewAndTheSavedViewStayApart) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  sensor::Sensor *first = scan.sensor(0);
  EXPECT_FALSE(scan.slot_pending(0));
  EXPECT_EQ(scan.saved_slots(), 2u);
  ASSERT_TRUE(scan.assign_and_save(0, ROM_B));  // a swap
  ASSERT_TRUE(scan.assign_and_save(3, ROM_C));  // past the bound slots
  ASSERT_TRUE(scan.forget_and_save(1));

  EXPECT_EQ(scan.address(0), ROM_A);
  EXPECT_EQ(scan.address(1), ROM_B);
  EXPECT_EQ(scan.address(3), 0u);
  EXPECT_EQ(scan.sensor(0), first);
  EXPECT_EQ(scan.sensor(3), nullptr);
  EXPECT_EQ(scan.slot_name(3), "Temp 4");
  EXPECT_EQ(scan.used_slots(), 2u);
  EXPECT_EQ(scan.sensors().size(), 2u);

  EXPECT_EQ(scan.saved_address(0), ROM_B);
  EXPECT_EQ(scan.saved_address(1), 0u);
  EXPECT_EQ(scan.saved_address(2), 0u);
  EXPECT_EQ(scan.saved_address(3), ROM_C);
  EXPECT_EQ(scan.saved_address(4), 0u);
  EXPECT_EQ(scan.saved_slots(), 4u);
  EXPECT_TRUE(scan.slot_pending(0));
  EXPECT_TRUE(scan.slot_pending(1));
  EXPECT_FALSE(scan.slot_pending(2));
  EXPECT_TRUE(scan.slot_pending(3));
  EXPECT_FALSE(scan.slot_pending(4));

  EXPECT_EQ(scan.check_assign(0, ROM_B), AssignCheck::UNCHANGED);
  EXPECT_FALSE(scan.can_forget(1));
  EXPECT_FALSE(scan.forget_and_save(1));
  EXPECT_TRUE(scan.can_forget(3));
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(Slots, ATablePutBackAsBootedLeavesNothingWaiting) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  EXPECT_FALSE(scan.reboot_required());
  ASSERT_TRUE(scan.assign_and_save(0, ROM_B));
  EXPECT_TRUE(scan.reboot_required());
  EXPECT_TRUE(this->log().has(this->log().infos, "0x8a0122791699dd28 takes slot 1 after a reboot"));
  this->log().clear();
  ASSERT_TRUE(scan.assign_and_save(0, ROM_A));  // swapped back
  EXPECT_FALSE(scan.reboot_required());
  EXPECT_TRUE(this->log().has(this->log().infos, "0xeb01227905460228 takes slot 1"));
  EXPECT_FALSE(this->log().has(this->log().infos, "after a reboot"));
  EXPECT_FALSE(scan.slot_pending(0));
  EXPECT_FALSE(scan.slot_pending(1));

  ASSERT_TRUE(scan.forget_and_save(1));
  EXPECT_TRUE(scan.reboot_required());
  ASSERT_TRUE(scan.assign_and_save(1, ROM_B));  // back in its slot
  EXPECT_FALSE(scan.reboot_required());
  EXPECT_EQ(this->read(), slot_file({{1, "0xeb01227905460228"}, {2, "0x8a0122791699dd28"}}));
  EXPECT_EQ(scan.restarts, 0);
}

}  // namespace esphome::dallas_scan::testing
