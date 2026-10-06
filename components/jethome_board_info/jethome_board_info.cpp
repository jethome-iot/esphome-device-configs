#include "jethome_board_info.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include "esphome/core/log.h"

namespace esphome::jethome_board_info {

static const char *const TAG = "jethome_board_info";

void JetHomeBoardInfo::setup() {
  if (this->eeprom_ == nullptr || this->eeprom_->is_failed()) {
    this->header_status_ = ReadStatus::NO_EEPROM;
    this->report_("not available");
    if (this->required_)
      this->mark_failed();
    return;
  }

  this->header_status_ = this->read_header_();
  if (this->header_status_ != ReadStatus::OK) {
    if (this->required_)
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
    this->report_("Unsupported filesystem version %u, ignoring the file area", this->data_.fs_version);
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
        ESP_LOGW(TAG, "%s: %s serial '%s' disagrees with the USID", this->where_(), DEVICE_ID_NAME,
                 this->device_serial_.c_str());
      }
    }
  }
}

ReadStatus JetHomeBoardInfo::read_header_() {
  auto *buf = reinterpret_cast<uint8_t *>(&this->data_);
  if (!this->eeprom_->get(0, buf, BOARD_DATA_SIZE)) {
    this->report_("Failed to read the %u-byte header", static_cast<unsigned>(BOARD_DATA_SIZE));
    return ReadStatus::NO_EEPROM;
  }

  const HeaderStatus status = validate_header(buf);
  switch (status) {
    case HeaderStatus::OK:
      this->crc_valid_ = true;
      break;
    case HeaderStatus::BAD_MAGIC:
      this->report_("Invalid magic");
      break;
    case HeaderStatus::UNSUPPORTED_VERSION:
      this->report_("Unsupported header version: %u", this->data_.version);
      break;
    case HeaderStatus::BAD_CRC:
      this->report_("CRC32 mismatch: computed=0x%08X stored=0x%08X",
                    static_cast<unsigned>(compute_crc32(buf, CRC_DATA_SIZE)), static_cast<unsigned>(this->data_.crc32));
      break;
  }
  return header_read_status(status);
}

ReadStatus JetHomeBoardInfo::find_file(const char *name, uint16_t &offset, uint16_t &size, uint32_t &crc32) {
  if (!this->valid_) {
    return this->header_status_;
  }

  JeefsWalk walk{};
  walk_begin(&walk, this->data_.fs_version, this->eeprom_->get_size(), name);

  uint8_t header[FILE_HEADER_SIZE];
  uint16_t at = 0;
  while (walk_want(&walk, &at)) {
    if (!this->eeprom_->get(at, header, FILE_HEADER_SIZE)) {
      this->report_("Failed to read a file header at 0x%04X", at);
      return ReadStatus::READ_ERROR;
    }
    walk_feed(&walk, header);
  }

  this->log_walk_status_(walk.status, name);
  const ReadStatus status = walk_read_status(walk.status, this->data_.fs_version);
  if (status != ReadStatus::OK) {
    return status;
  }
  offset = static_cast<uint16_t>(walk.file_offset);
  size = walk.file_size;
  crc32 = walk.file_crc32;
  return ReadStatus::OK;
}

ReadStatus JetHomeBoardInfo::read_file(const char *name, uint8_t *data, size_t size, uint16_t *read_size) {
  uint16_t offset = 0;
  uint16_t file_size = 0;
  uint32_t crc32 = 0;
  const ReadStatus found = this->find_file(name, offset, file_size, crc32);
  if (found != ReadStatus::OK) {
    return found;
  }
  if (file_size > size) {
    this->report_("%s is %u bytes, the buffer holds %u", name, file_size, static_cast<unsigned>(size));
    return ReadStatus::TOO_LARGE;
  }
  if (!this->eeprom_->get(offset, data, file_size)) {
    this->report_("Failed to read %s", name);
    return ReadStatus::READ_ERROR;
  }
  if (compute_crc32(data, file_size) != crc32) {
    this->report_("%s file CRC32 mismatch", name);
    return ReadStatus::CORRUPT;
  }
  if (read_size != nullptr) {
    *read_size = file_size;
  }
  return ReadStatus::OK;
}

bool JetHomeBoardInfo::read_device_identity_() {
  uint16_t offset = 0;
  uint16_t size = 0;
  uint32_t crc32 = 0;
  if (this->find_file(DEVICE_ID_NAME, offset, size, crc32) != ReadStatus::OK) {
    return false;
  }
  if (size != DEVICE_ID_SIZE) {
    this->report_("%s is not %u bytes", DEVICE_ID_NAME, static_cast<unsigned>(DEVICE_ID_SIZE));
    return false;
  }

  auto *buf = reinterpret_cast<uint8_t *>(&this->devid_);
  if (!this->eeprom_->get(offset, buf, DEVICE_ID_SIZE)) {
    this->report_("Failed to read %s", DEVICE_ID_NAME);
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
      ESP_LOGI(TAG, "%s: no %s in the file chain", this->where_(), name);
      return;
    case WalkStatus::NO_FILESYSTEM:
      if (this->data_.fs_version == FS_VERSION_NONE) {
        ESP_LOGI(TAG, "%s: no filesystem, so there is no %s", this->where_(), name);
      } else {
        this->report_("Unsupported filesystem version %u, so %s cannot be read", this->data_.fs_version, name);
      }
      return;
    case WalkStatus::BAD_NAME:
      this->report_("'%s' is not a name a JEEFS file can have", name == nullptr ? "(null)" : name);
      return;
    case WalkStatus::BAD_FILE_HEADER_CRC:
      this->report_("File header CRC32 mismatch while looking for %s", name);
      return;
    case WalkStatus::CORRUPT_CHAIN:
      this->report_("File chain is corrupt while looking for %s", name);
      return;
  }
}

void JetHomeBoardInfo::log_record_status_(RecordStatus status) {
  switch (status) {
    case RecordStatus::OK:
      return;
    case RecordStatus::BAD_FILE_DATA_CRC:
      this->report_("%s file CRC32 mismatch", DEVICE_ID_NAME);
      return;
    case RecordStatus::BAD_MAGIC:
      this->report_("Invalid %s magic", DEVICE_ID_NAME);
      return;
    case RecordStatus::UNSUPPORTED_VERSION:
      this->report_("Unsupported device identity version: %u", this->devid_.record_version);
      return;
    case RecordStatus::UNSUPPORTED_SIGNATURE:
      this->report_("Unsupported %s signature version: %u", DEVICE_ID_NAME, this->devid_.signature_version);
      return;
    case RecordStatus::BAD_RECORD_CRC:
      this->report_("%s record CRC32 mismatch", DEVICE_ID_NAME);
      return;
  }
}

// Two boards can each carry an EEPROM, and every instance logs under the same tag.
const char *JetHomeBoardInfo::where_() {
  if (this->eeprom_ == nullptr)
    return "EEPROM";
  snprintf(this->where_buf_, sizeof(this->where_buf_), "EEPROM 0x%02X", this->eeprom_->get_i2c_address());
  return this->where_buf_;
}

void JetHomeBoardInfo::report_(const char *format, ...) {
  char message[128];
  va_list args;
  va_start(args, format);
  vsnprintf(message, sizeof(message), format, args);
  va_end(args);
  if (this->required_) {
    ESP_LOGE(TAG, "%s: %s", this->where_(), message);
  } else {
    ESP_LOGW(TAG, "%s: %s", this->where_(), message);
  }
}

void JetHomeBoardInfo::dump_config() {
  ESP_LOGCONFIG(TAG, "JetHome Board Info:");
  if (this->eeprom_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  EEPROM: 0x%02X%s", this->eeprom_->get_i2c_address(), this->required_ ? "" : " (optional)");
  }
  if (!this->valid_) {
    ESP_LOGCONFIG(TAG, "  Status: %s", read_status_str(this->header_status_));
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
