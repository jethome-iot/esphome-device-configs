#pragma once

#include <cstdint>
#include <string>

namespace esphome::firmware_rollback {

/// The app slot a rollback would boot, empty when there is none.
struct RollbackTarget {
  std::string partition;
  std::string version;
  std::string project_name;
  bool available() const { return !this->partition.empty(); }
};

/// The other slot's otadata state, numbered as esp_ota_img_states_t, plus NO_ENTRY for a slot
/// no otadata entry selects.
enum class SlotState : uint32_t {
  NEW = 0,
  PENDING_VERIFY = 1,
  VALID = 2,
  INVALID = 3,
  ABORTED = 4,
  NO_ENTRY = 0xFFFFFFFE,
  UNDEFINED = 0xFFFFFFFF,
};

/// What rollback_target() reads off the flash before it decides.
struct OtherSlot {
  /// nullptr when there is no second OTA slot.
  const char *partition{nullptr};
  /// The next boot is already not the running firmware.
  bool switch_pending{false};
  SlotState state{SlotState::NO_ENTRY};
  /// The app descriptor's 32-byte fields, unterminated when full; nullptr without a descriptor.
  const char *version{nullptr};
  const char *project_name{nullptr};
};

/// A target only when the bootloader would boot the other slot and nothing is pending.
RollbackTarget evaluate(const OtherSlot &slot);

/// Reads otadata and the app descriptor, never the image, so any task may ask.
RollbackTarget rollback_target();

/// Makes @p target the next boot: nullptr on success, else why not. Two selects must not
/// overlap, so every caller uses the loop task, which the image hash stalls for a moment; the
/// caller reboots.
const char *select_rollback(const RollbackTarget &target);

}  // namespace esphome::firmware_rollback
