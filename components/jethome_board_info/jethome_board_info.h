#pragma once
#include <cstdint>
#include <string>
#include "esphome/components/i2c_eeprom/i2c_eeprom.h"
#include "esphome/core/component.h"
#include "jeefs_parse.h"

namespace esphome::jethome_board_info {

// Reads the board header and the device.id record once at boot and keeps what they say,
// signature included. Nothing is verified here: that takes JetHome's public key and the chip's
// eFuse MACs, and belongs to whoever reads this out of the device.
class JetHomeBoardInfo : public Component {
 public:
  void setup() override;
  void dump_config() override;
  // Once the EEPROM (DATA) is up.
  float get_setup_priority() const override { return setup_priority::DATA - 1.0f; }

  void set_eeprom(i2c_eeprom::I2CEeprom *eeprom) { this->eeprom_ = eeprom; }

  // Locates a file by walking the chain; outputs where its data starts, its length and its CRC32.
  bool find_file(const char *name, uint16_t &offset, uint16_t &size, uint32_t &crc32);
  // Reads a file's data into `data` and verifies it against the CRC32 the chain records.
  bool read_file(const char *name, uint8_t *data, size_t size, uint16_t *read_size = nullptr);

  bool is_valid() const { return this->valid_; }
  bool is_crc_valid() const { return this->crc_valid_; }
  bool has_device_identity() const { return this->devid_valid_; }
  // Cross-check of device.id against the signed USID; the one check that needs no key.
  bool has_serial_check() const { return this->serial_checked_; }
  bool serial_matches_usid() const { return this->serial_match_; }

  const std::string &get_boardname() const { return this->boardname_; }
  const std::string &get_boardversion() const { return this->boardversion_; }
  // Board-scoped, so empty on v3, whose header field holds the device serial instead.
  const std::string &get_board_serial() const { return this->board_serial_; }
  // From device.id, falling back to the v3 header field.
  const std::string &get_device_serial() const { return this->device_serial_; }
  const std::string &get_device_model() const { return this->device_model_; }
  const std::string &get_hw_revision() const { return this->hw_revision_; }
  const std::string &get_usid() const { return this->usid_; }
  const std::string &get_cpuid() const { return this->cpuid_; }
  const std::string &get_mac_str() const { return this->mac_str_; }
  const uint8_t *get_mac() const { return this->data_.mac; }
  int64_t get_timestamp() const { return this->data_.timestamp; }
  uint8_t get_header_version() const { return this->data_.version; }

  // The signature that covers this device: the record's once there is one, else the header's.
  // ECDSA_P256_SIG_SIZE bytes of r || s over signing_payload(cpuid, mac, usid) when the
  // algorithm is SIG_SECP256R1; SIG_NONE means the device was never signed.
  uint8_t get_signature_version() const;
  const uint8_t *get_signature() const;

 protected:
  bool read_header_();
  bool read_device_identity_();
  void log_walk_status_(WalkStatus status, const char *name);
  void log_record_status_(RecordStatus status);

  i2c_eeprom::I2CEeprom *eeprom_{nullptr};
  JetHomeBoardData data_{};
  JetHomeDeviceIdentity devid_{};
  bool valid_{false};
  bool crc_valid_{false};
  bool devid_valid_{false};
  bool serial_checked_{false};
  bool serial_match_{false};

  std::string boardname_;
  std::string boardversion_;
  std::string board_serial_;
  std::string device_serial_;
  std::string device_model_;
  std::string hw_revision_;
  std::string usid_;
  std::string cpuid_;
  std::string mac_str_;
};

}  // namespace esphome::jethome_board_info
