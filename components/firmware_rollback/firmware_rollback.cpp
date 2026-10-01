#include "firmware_rollback.h"
#include <cstring>
#include "esphome/core/log.h"
#ifdef USE_ESP32
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#endif

namespace esphome::firmware_rollback {

static const char *const TAG = "firmware_rollback";

// The width of esp_app_desc_t's version and project_name.
static constexpr size_t DESC_FIELD_LEN = 32;

// The states IDF's own bootloader_common_ota_select_valid() would still boot: an INVALID or
// ABORTED slot is skipped at boot, and NO_ENTRY is a slot no otadata entry selects.
static bool bootable(SlotState state) {
  switch (state) {
    case SlotState::NEW:
    case SlotState::PENDING_VERIFY:
    case SlotState::VALID:
    case SlotState::UNDEFINED:
      return true;
    default:
      return false;
  }
}

RollbackTarget evaluate(const OtherSlot &slot) {
  RollbackTarget target;
  if (slot.partition == nullptr || slot.switch_pending || !bootable(slot.state) || slot.version == nullptr ||
      slot.project_name == nullptr)
    return target;
  target.partition = slot.partition;
  target.version.assign(slot.version, strnlen(slot.version, DESC_FIELD_LEN));
  target.project_name.assign(slot.project_name, strnlen(slot.project_name, DESC_FIELD_LEN));
  return target;
}

#ifdef USE_ESP32
static_assert(static_cast<uint32_t>(SlotState::NEW) == ESP_OTA_IMG_NEW);
static_assert(static_cast<uint32_t>(SlotState::PENDING_VERIFY) == ESP_OTA_IMG_PENDING_VERIFY);
static_assert(static_cast<uint32_t>(SlotState::VALID) == ESP_OTA_IMG_VALID);
static_assert(static_cast<uint32_t>(SlotState::INVALID) == ESP_OTA_IMG_INVALID);
static_assert(static_cast<uint32_t>(SlotState::ABORTED) == ESP_OTA_IMG_ABORTED);
static_assert(static_cast<uint32_t>(SlotState::UNDEFINED) == ESP_OTA_IMG_UNDEFINED);
static_assert(sizeof(esp_app_desc_t::version) == DESC_FIELD_LEN);
static_assert(sizeof(esp_app_desc_t::project_name) == DESC_FIELD_LEN);

RollbackTarget rollback_target() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (other == nullptr || other->address == running->address)
    return {};
  OtherSlot slot;
  slot.partition = other->label;
  const esp_partition_t *boot = esp_ota_get_boot_partition();
  slot.switch_pending = boot == nullptr || boot->address != running->address;
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(other, &state) == ESP_OK)
    slot.state = static_cast<SlotState>(state);
  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(other, &desc) == ESP_OK) {
    slot.version = desc.version;
    slot.project_name = desc.project_name;
  }
  return evaluate(slot);
}

// A firmware still in its monitored first boot is confirmed first: left pending, a reset
// before the target confirms itself would mark it ABORTED, and a target that then fails
// has nothing to fall back to. Only a target that verifies is worth that.
static esp_err_t switch_to(const esp_partition_t *other) {
  esp_ota_img_states_t running;
  if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &running) == ESP_OK &&
      running == ESP_OTA_IMG_PENDING_VERIFY) {
    const esp_partition_pos_t pos = {.offset = other->address, .size = other->size};
    esp_image_metadata_t data = {};
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &data);
    if (err == ESP_OK)
      err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK)
      return err;
  }
  return esp_ota_set_boot_partition(other);
}

const char *select_rollback(const RollbackTarget &target) {
  const RollbackTarget now = rollback_target();
  const char *error = "The firmware to roll back to is gone";
  if (target.available() && now.partition == target.partition) {
    const esp_err_t err = switch_to(esp_ota_get_next_update_partition(nullptr));
    error = err == ESP_OK ? nullptr : esp_err_to_name(err);
  }
  if (error != nullptr) {
    if (target.available()) {
      ESP_LOGE(TAG, "Rollback to '%s' failed: %s", target.partition.c_str(), error);
    } else {
      ESP_LOGE(TAG, "Rollback failed: %s", error);
    }
    return error;
  }
  ESP_LOGW(TAG, "Rolling back to '%s'", target.partition.c_str());
  return nullptr;
}
#else
RollbackTarget rollback_target() { return {}; }

const char *select_rollback(const RollbackTarget & /*target*/) { return "Rollback needs an ESP32"; }
#endif

void FirmwareRollback::setup() { this->refresh(); }

void FirmwareRollback::dump_config() {
  ESP_LOGCONFIG(TAG, "Firmware Rollback:");
  if (this->target_.available()) {
    ESP_LOGCONFIG(TAG, "  Other slot: %s, version %s", this->target_.partition.c_str(), this->target_.version.c_str());
  } else {
    ESP_LOGCONFIG(TAG, "  Other slot: nothing to roll back to");
  }
}

void FirmwareRollback::refresh() { this->target_ = rollback_target(); }

const char *FirmwareRollback::rollback() {
  const char *error = select_rollback(this->target_);
  if (error != nullptr)
    this->refresh();
  return error;
}

}  // namespace esphome::firmware_rollback
