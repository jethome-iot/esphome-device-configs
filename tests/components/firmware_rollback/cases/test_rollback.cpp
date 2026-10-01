#include <gtest/gtest.h>
#include <cstring>
#include <memory>
#include "esphome/components/firmware_rollback/firmware_rollback.h"

namespace esphome::firmware_rollback::testing {

// The other slot as a board that has just been updated over the air sees it: the firmware
// this one replaced, confirmed when it last ran.
static OtherSlot replaced() {
  OtherSlot slot;
  slot.partition = "app1";
  slot.state = SlotState::VALID;
  slot.version = "2026.8.1";
  slot.project_name = "jxd-r6-e1eth-lcd";
  return slot;
}

TEST(Evaluate, ABootableSlotWithADescriptorIsATarget) {
  const RollbackTarget target = evaluate(replaced());
  EXPECT_TRUE(target.available());
  EXPECT_EQ(target.partition, "app1");
  EXPECT_EQ(target.version, "2026.8.1");
  EXPECT_EQ(target.project_name, "jxd-r6-e1eth-lcd");
}

// What IDF's bootloader would still boot: a slot never booted, one that was booted and not
// yet confirmed, and one written without rollback support.
TEST(Evaluate, AcceptsEveryStateTheBootloaderWouldBoot) {
  for (SlotState state : {SlotState::NEW, SlotState::PENDING_VERIFY, SlotState::UNDEFINED}) {
    OtherSlot slot = replaced();
    slot.state = state;
    EXPECT_EQ(evaluate(slot).partition, "app1") << static_cast<uint32_t>(state);
  }
}

// A failed or interrupted update, a firmware the bootloader rolled back from, and a slot no
// otadata entry selects, as after a serial flash.
TEST(Evaluate, RefusesASlotTheBootloaderWouldSkip) {
  for (SlotState state : {SlotState::NO_ENTRY, SlotState::INVALID, SlotState::ABORTED}) {
    OtherSlot slot = replaced();
    slot.state = state;
    const RollbackTarget target = evaluate(slot);
    EXPECT_FALSE(target.available()) << static_cast<uint32_t>(state);
    EXPECT_TRUE(target.version.empty()) << static_cast<uint32_t>(state);
  }
}

TEST(Evaluate, RefusesWhileASwitchWaitsForItsReboot) {
  OtherSlot slot = replaced();
  slot.switch_pending = true;
  EXPECT_FALSE(evaluate(slot).available());
}

TEST(Evaluate, RefusesASlotWithoutADescriptor) {
  OtherSlot slot = replaced();
  slot.version = nullptr;
  slot.project_name = nullptr;
  EXPECT_FALSE(evaluate(slot).available());
}

TEST(Evaluate, RefusesWithoutASecondSlot) {
  OtherSlot slot = replaced();
  slot.partition = nullptr;
  EXPECT_FALSE(evaluate(slot).available());
  EXPECT_FALSE(evaluate(OtherSlot{}).available());
}

// A descriptor field that fills its 32 bytes has no terminator. Heap copies of exactly that
// size, so ASan reports a read past either one.
TEST(Evaluate, ReadsAFullDescriptorFieldToItsEndAndNoFurther) {
  const std::string version(32, 'v');
  const std::string project(32, 'p');
  auto version_field = std::make_unique<char[]>(32);
  auto project_field = std::make_unique<char[]>(32);
  memcpy(version_field.get(), version.data(), 32);
  memcpy(project_field.get(), project.data(), 32);
  OtherSlot slot = replaced();
  slot.version = version_field.get();
  slot.project_name = project_field.get();
  const RollbackTarget target = evaluate(slot);
  EXPECT_EQ(target.version, version);
  EXPECT_EQ(target.project_name, project);
}

TEST(HostBuild, HasNothingToRollBackTo) { EXPECT_FALSE(rollback_target().available()); }

TEST(HostBuild, RefusesToSelect) {
  RollbackTarget target;
  target.partition = "app1";
  EXPECT_STREQ(select_rollback(target), "Rollback needs an ESP32");
}

}  // namespace esphome::firmware_rollback::testing
