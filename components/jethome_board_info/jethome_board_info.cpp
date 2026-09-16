#include "jethome_board_info.h"
#include <cstring>
#include <ctime>
#include "esphome/core/log.h"

namespace esphome::jethome_board_info {

static const char *const TAG = "jethome_board_info";

void JetHomeBoardInfo::setup() {
  if (this->eeprom_ == nullptr || this->eeprom_->is_failed()) {
    ESP_LOGE(TAG, "EEPROM not available");
    this->mark_failed();
    return;
  }

  if (!this->read_header_()) {
    this->mark_failed();
    return;
  }

  this->boardname_ = field_to_string(this->data_.boardname, sizeof(this->data_.boardname));
  this->boardversion_ = field_to_string(this->data_.boardversion, sizeof(this->data_.boardversion));
  this->usid_ = field_to_string(this->data_.usid, sizeof(this->data_.usid));
  this->cpuid_ = field_to_string(this->data_.cpuid, sizeof(this->data_.cpuid));
  this->mac_str_ = format_mac(this->data_.mac);

  // v3 kept the device serial in the field v4 renamed to board_serial and now leaves empty.
  std::string header_serial = field_to_string(this->data_.board_serial, sizeof(this->data_.board_serial));
  if (this->data_.version == HEADER_VERSION_V4) {
    this->board_serial_ = header_serial;
  } else {
    this->device_serial_ = header_serial;
  }

  this->valid_ = true;

  // Deliberately not gated on the header version: fs_version is a named field in v3 too.
  if (this->data_.fs_version == FS_VERSION_JEEFS_V1) {
    this->devid_valid_ = this->read_device_identity_();
  } else if (this->data_.fs_version != FS_VERSION_NONE) {
    ESP_LOGE(TAG, "Unsupported filesystem version %u, ignoring the file area", this->data_.fs_version);
  }

  if (this->devid_valid_) {
    this->device_model_ = field_to_string(this->devid_.device_model, sizeof(this->devid_.device_model));
    this->hw_revision_ = field_to_string(this->devid_.hw_revision, sizeof(this->devid_.hw_revision));
    // An empty field is not an answer: on v3 it would discard the serial the header supplied.
    std::string record_serial = field_to_string(this->devid_.device_serial, sizeof(this->devid_.device_serial));
    if (!record_serial.empty()) {
      this->device_serial_ = record_serial;
    }

    this->serial_checked_ = this->usid_.size() == USID_V2_LENGTH && !this->device_serial_.empty();
    if (this->serial_checked_) {
      this->serial_match_ = usid_encodes_serial(this->usid_, this->device_serial_);
      if (!this->serial_match_) {
        ESP_LOGW(TAG, "%s serial '%s' disagrees with the USID", DEVICE_ID_NAME, this->device_serial_.c_str());
      }
    }
  }
}

bool JetHomeBoardInfo::read_header_() {
  auto *buf = reinterpret_cast<uint8_t *>(&this->data_);
  if (!this->eeprom_->get(0, buf, BOARD_DATA_SIZE)) {
    ESP_LOGE(TAG, "Failed to read %u bytes from EEPROM", static_cast<unsigned>(BOARD_DATA_SIZE));
    return false;
  }

  switch (validate_header(buf)) {
    case HeaderStatus::OK:
      this->crc_valid_ = true;
      return true;
    case HeaderStatus::BAD_MAGIC:
      ESP_LOGE(TAG, "Invalid magic");
      return false;
    case HeaderStatus::UNSUPPORTED_VERSION:
      ESP_LOGE(TAG, "Unsupported header version: %u", this->data_.version);
      return false;
    case HeaderStatus::BAD_CRC:
      ESP_LOGE(TAG, "CRC32 mismatch: computed=0x%08X stored=0x%08X", compute_crc32(buf, CRC_DATA_SIZE),
               this->data_.crc32);
      return false;
  }
  return false;
}

bool JetHomeBoardInfo::find_file(const char *name, uint16_t &offset, uint16_t &size, uint32_t &crc32) {
  if (!this->valid_) {
    return false;
  }

  JeefsWalk walk{};
  walk_begin(&walk, this->data_.fs_version, this->eeprom_->get_size(), name);

  uint8_t header[FILE_HEADER_SIZE];
  uint16_t at = 0;
  while (walk_want(&walk, &at)) {
    if (!this->eeprom_->get(at, header, FILE_HEADER_SIZE)) {
      ESP_LOGE(TAG, "Failed to read a file header at 0x%04X", at);
      return false;
    }
    walk_feed(&walk, header);
  }

  this->log_walk_status_(walk.status, name);
  if (walk.status != WalkStatus::FOUND) {
    return false;
  }
  offset = static_cast<uint16_t>(walk.file_offset);
  size = walk.file_size;
  crc32 = walk.file_crc32;
  return true;
}

bool JetHomeBoardInfo::read_file(const char *name, uint8_t *data, size_t size, uint16_t *read_size) {
  uint16_t offset = 0;
  uint16_t file_size = 0;
  uint32_t crc32 = 0;
  if (!this->find_file(name, offset, file_size, crc32)) {
    return false;
  }
  if (file_size > size) {
    ESP_LOGE(TAG, "%s is %u bytes, the buffer holds %u", name, file_size, static_cast<unsigned>(size));
    return false;
  }
  if (!this->eeprom_->get(offset, data, file_size)) {
    ESP_LOGE(TAG, "Failed to read %s", name);
    return false;
  }
  if (compute_crc32(data, file_size) != crc32) {
    ESP_LOGE(TAG, "%s file CRC32 mismatch", name);
    return false;
  }
  if (read_size != nullptr) {
    *read_size = file_size;
  }
  return true;
}

bool JetHomeBoardInfo::read_device_identity_() {
  uint16_t offset = 0;
  uint16_t size = 0;
  uint32_t crc32 = 0;
  if (!this->find_file(DEVICE_ID_NAME, offset, size, crc32)) {
    return false;
  }
  if (size != DEVICE_ID_SIZE) {
    ESP_LOGE(TAG, "%s is not %u bytes", DEVICE_ID_NAME, static_cast<unsigned>(DEVICE_ID_SIZE));
    return false;
  }

  auto *buf = reinterpret_cast<uint8_t *>(&this->devid_);
  if (!this->eeprom_->get(offset, buf, DEVICE_ID_SIZE)) {
    ESP_LOGE(TAG, "Failed to read %s", DEVICE_ID_NAME);
    return false;
  }

  RecordStatus status = validate_record(buf, crc32);
  if (status != RecordStatus::OK) {
    this->log_record_status_(status);
    return false;
  }

  return true;
}

void JetHomeBoardInfo::log_walk_status_(WalkStatus status, const char *name) {
  switch (status) {
    case WalkStatus::WANT_HEADER:
    case WalkStatus::FOUND:
      return;
    case WalkStatus::NOT_FOUND:
      ESP_LOGI(TAG, "No %s in the file chain", name);
      return;
    case WalkStatus::NO_FILESYSTEM:
      ESP_LOGI(TAG, "No filesystem, so there is no %s", name);
      return;
    case WalkStatus::BAD_NAME:
      ESP_LOGE(TAG, "'%s' is not a name a JEEFS file can have", name == nullptr ? "(null)" : name);
      return;
    case WalkStatus::BAD_FILE_HEADER_CRC:
      ESP_LOGE(TAG, "File header CRC32 mismatch while looking for %s", name);
      return;
    case WalkStatus::CORRUPT_CHAIN:
      ESP_LOGE(TAG, "File chain is corrupt while looking for %s", name);
      return;
  }
}

void JetHomeBoardInfo::log_record_status_(RecordStatus status) {
  switch (status) {
    case RecordStatus::OK:
      return;
    case RecordStatus::BAD_FILE_DATA_CRC:
      ESP_LOGE(TAG, "%s file CRC32 mismatch", DEVICE_ID_NAME);
      return;
    case RecordStatus::BAD_MAGIC:
      ESP_LOGE(TAG, "Invalid %s magic", DEVICE_ID_NAME);
      return;
    case RecordStatus::UNSUPPORTED_VERSION:
      ESP_LOGE(TAG, "Unsupported device identity version: %u", this->devid_.record_version);
      return;
    case RecordStatus::UNSUPPORTED_SIGNATURE:
      ESP_LOGE(TAG, "Unsupported %s signature version: %u", DEVICE_ID_NAME, this->devid_.signature_version);
      return;
    case RecordStatus::BAD_RECORD_CRC:
      ESP_LOGE(TAG, "%s record CRC32 mismatch", DEVICE_ID_NAME);
      return;
  }
}

void JetHomeBoardInfo::dump_config() {
  ESP_LOGCONFIG(TAG, "JetHome Board Info:");
  if (!this->valid_) {
    ESP_LOGCONFIG(TAG, "  Status: INVALID");
    return;
  }
  ESP_LOGCONFIG(TAG, "  Header version: %u", this->data_.version);
  ESP_LOGCONFIG(TAG, "  Board: %s", this->boardname_.c_str());
  ESP_LOGCONFIG(TAG, "  Version: %s", this->boardversion_.c_str());
  // Either can be absent: v3 has no board serial, and a v4 board may carry no device.id.
  if (!this->board_serial_.empty()) {
    ESP_LOGCONFIG(TAG, "  Board serial: %s", this->board_serial_.c_str());
  }
  if (!this->device_serial_.empty()) {
    ESP_LOGCONFIG(TAG, "  Device serial: %s", this->device_serial_.c_str());
  }
  ESP_LOGCONFIG(TAG, "  USID: %s", this->usid_.c_str());
  ESP_LOGCONFIG(TAG, "  CPU ID: %s", this->cpuid_.c_str());
  ESP_LOGCONFIG(TAG, "  MAC: %s", this->mac_str_.c_str());
  {
    time_t ts = static_cast<time_t>(this->data_.timestamp);
    struct tm *tm_info = gmtime(&ts);
    if (tm_info != nullptr) {
      char time_buf[32];
      strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S UTC", tm_info);
      ESP_LOGCONFIG(TAG, "  Timestamp: %s", time_buf);
    } else {
      ESP_LOGCONFIG(TAG, "  Timestamp: invalid (%ld)", static_cast<long>(ts));
    }
  }
  ESP_LOGCONFIG(TAG, "  Filesystem version: %u", this->data_.fs_version);
  if (this->devid_valid_) {
    ESP_LOGCONFIG(TAG, "  Device model: %s", this->device_model_.c_str());
    ESP_LOGCONFIG(TAG, "  HW revision: %s", this->hw_revision_.c_str());
    if (this->serial_checked_) {
      ESP_LOGCONFIG(TAG, "  Serial vs USID: %s", this->serial_match_ ? "MATCH" : "MISMATCH");
    }
  } else {
    ESP_LOGCONFIG(TAG, "  Device identity: ABSENT");
  }
  ESP_LOGCONFIG(TAG, "  Signature version: %u", this->get_signature_version());
}

// Only the location of the 64 bytes moved in v4. The record is what we publish, so once there
// is one, the header's signature is no stand-in for the record's own.
uint8_t JetHomeBoardInfo::get_signature_version() const {
  return this->devid_valid_ ? this->devid_.signature_version : this->data_.signature_version;
}

const uint8_t *JetHomeBoardInfo::get_signature() const {
  return this->devid_valid_ ? this->devid_.signature : this->data_.signature;
}

}  // namespace esphome::jethome_board_info
