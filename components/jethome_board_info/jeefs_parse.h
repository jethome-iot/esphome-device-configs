#pragma once
// Byte-level JEEFS parsing, deliberately free of ESPHome, mbedtls, eFuse and I2C so the
// host tests in tests/components/jethome_board_info/ can include this header on its own.
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace esphome::jethome_board_info {

static const size_t BOARD_DATA_SIZE = 256;
static const size_t CRC_DATA_SIZE = 252;
static const size_t ECDSA_P256_SIG_SIZE = 64;
static const size_t FILE_HEADER_SIZE = 28;
static const size_t FILE_HEADER_CRC_DATA_SIZE = 24;
static const size_t FILE_NAME_SIZE = 16;
// An erased link reads 0xFFFF and terminates the chain exactly like 0.
static const uint16_t NEXT_TERMINAL = 0xFFFF;
static const size_t DEVICE_ID_SIZE = 256;
static const size_t DEVICE_ID_CRC_DATA_SIZE = 252;

// The chain starts right after the board header. device.id is written first, but nothing here
// relies on that — the walker follows the links.
static const uint16_t FILE_HEADER_ADDR = BOARD_DATA_SIZE;

// USID v2: {model:6}{hwrev:2}{pfrev:2}{yymm:4}{site:2}{serial:10}{checksum:4}.
static const size_t USID_V2_LENGTH = 30;
static const size_t USID_SERIAL_OFFSET = 16;
static const size_t USID_SERIAL_LENGTH = 10;

static const char BOARD_MAGIC[] = "JETHOME";
static const char DEVID_MAGIC[] = "JHDEVID";
static const char DEVICE_ID_NAME[] = "device.id";
static const uint8_t HEADER_VERSION_V3 = 3;
static const uint8_t HEADER_VERSION_V4 = 4;
static const uint8_t DEVID_RECORD_VERSION = 1;
static const uint8_t FS_VERSION_NONE = 0;
static const uint8_t FS_VERSION_JEEFS_V1 = 1;
static const uint8_t SIG_NONE = 0;
static const uint8_t SIG_SECP256R1 = 2;
// Both count as "nothing written": zero-padding inside structures, 0xFF on erased medium.
static const uint8_t EMPTY_BYTE = 0x00;
static const uint8_t ERASED_BYTE = 0xFF;

struct __attribute__((packed)) JetHomeBoardData {
  char magic[8];              // 0-7: "JETHOME\0"
  uint8_t version;            // 8
  uint8_t signature_version;  // 9
  uint8_t fs_version;         // 10: 0 = no filesystem, 1 = JEEFS v1
  uint8_t header_reserved;    // 11
  char boardname[32];         // 12-43
  char boardversion[32];      // 44-75
  char board_serial[32];      // 76-107: board-scoped in v4, the device serial in v3
  char usid[32];              // 108-139
  char cpuid[32];             // 140-171
  uint8_t mac[6];             // 172-177
  uint8_t reserved2[2];       // 178-179
  uint8_t signature[64];      // 180-243: zero on v4, which signs device.id instead
  int64_t timestamp;          // 244-251
  uint32_t crc32;             // 252-255
};

static_assert(sizeof(JetHomeBoardData) == BOARD_DATA_SIZE, "JetHomeBoardData must be 256 bytes");

struct __attribute__((packed)) JeefsFileHeader {
  char name[16];               // 0-15
  uint16_t data_size;          // 16-17
  uint32_t crc32;              // 18-21: over the file data
  uint16_t next_file_address;  // 22-23
  uint32_t header_crc32;       // 24-27: over bytes 0-23
};

static_assert(sizeof(JeefsFileHeader) == FILE_HEADER_SIZE, "JeefsFileHeader must be 28 bytes");

struct __attribute__((packed)) JetHomeDeviceIdentity {
  char magic[8];              // 0-7: "JHDEVID\0"
  uint8_t record_version;     // 8
  uint8_t signature_version;  // 9
  uint8_t reserved1[2];       // 10-11
  char device_model[32];      // 12-43
  char device_serial[32];     // 44-75
  char hw_revision[16];       // 76-91
  uint16_t flags;             // 92-93
  uint8_t reserved2[86];      // 94-179
  uint8_t signature[64];      // 180-243
  int64_t timestamp;          // 244-251
  uint32_t crc32;             // 252-255
};

static_assert(sizeof(JetHomeDeviceIdentity) == DEVICE_ID_SIZE, "JetHomeDeviceIdentity must be 256 bytes");

enum class HeaderStatus : uint8_t {
  OK,
  BAD_MAGIC,
  UNSUPPORTED_VERSION,
  BAD_CRC,
};

// How a walk over the file chain ended; what the located file contains is RecordStatus.
enum class WalkStatus : uint8_t {
  WANT_HEADER,
  FOUND,
  NOT_FOUND,
  NO_FILESYSTEM,
  BAD_NAME,
  BAD_FILE_HEADER_CRC,
  CORRUPT_CHAIN,
};

enum class RecordStatus : uint8_t {
  OK,
  BAD_FILE_DATA_CRC,
  BAD_MAGIC,
  UNSUPPORTED_VERSION,
  UNSUPPORTED_SIGNATURE,
  BAD_RECORD_CRC,
};

// Pull model: the walker owns the state machine, the caller owns every read. Offsets are
// 32-bit so a link at the top of a 64 KiB image cannot wrap while its end is computed.
struct JeefsWalk {
  uint32_t want_offset;
  uint32_t file_offset;
  uint32_t file_crc32;
  uint32_t image_size;
  uint16_t file_size;
  WalkStatus status;
  char target[FILE_NAME_SIZE];
};

// IEEE 802.3 / zlib CRC-32, the one every JEEFS structure is checked with.
inline uint32_t compute_crc32(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
    }
  }
  return ~crc;
}

// v1 is 512 bytes and v2 has no signature, so parsing either as v3/v4 would invent fields.
inline HeaderStatus validate_header(const uint8_t *raw) {
  const auto *data = reinterpret_cast<const JetHomeBoardData *>(raw);
  if (memcmp(data->magic, BOARD_MAGIC, sizeof(BOARD_MAGIC)) != 0) {
    return HeaderStatus::BAD_MAGIC;
  }
  if (data->version != HEADER_VERSION_V3 && data->version != HEADER_VERSION_V4) {
    return HeaderStatus::UNSUPPORTED_VERSION;
  }
  if (compute_crc32(raw, CRC_DATA_SIZE) != data->crc32) {
    return HeaderStatus::BAD_CRC;
  }
  return HeaderStatus::OK;
}

// A name must fit with its terminator: truncating it would compare equal to a shorter file
// on disk and hand the caller a file it never asked for.
inline bool file_name_fits(const char *name) {
  if (name == nullptr) {
    return false;
  }
  size_t len = strnlen(name, FILE_NAME_SIZE);
  return len > 0 && len < FILE_NAME_SIZE;
}

// Starts a search for `name`. The header is already validated by the caller, so only the byte
// that says whether there is a filesystem at all is needed here.
inline WalkStatus walk_begin(JeefsWalk *w, uint8_t fs_version, uint32_t image_size, const char *name) {
  *w = JeefsWalk{};
  w->image_size = image_size;
  if (!file_name_fits(name)) {
    return w->status = WalkStatus::BAD_NAME;
  }
  strncpy(w->target, name, sizeof(w->target) - 1);
  if (fs_version != FS_VERSION_JEEFS_V1) {
    return w->status = WalkStatus::NO_FILESYSTEM;
  }
  if (FILE_HEADER_ADDR + FILE_HEADER_SIZE > image_size) {
    return w->status = WalkStatus::NOT_FOUND;
  }
  w->want_offset = FILE_HEADER_ADDR;
  return w->status = WalkStatus::WANT_HEADER;
}

// True while the caller still owes the walker a FILE_HEADER_SIZE read at *offset.
inline bool walk_want(const JeefsWalk *w, uint16_t *offset) {
  if (w->status != WalkStatus::WANT_HEADER) {
    return false;
  }
  *offset = static_cast<uint16_t>(w->want_offset);
  return true;
}

// One slot's worth of validation, in the order upstream's walker applies it: nothing is
// trusted before the CRC, and a link that does not abut its own file ends the walk.
inline WalkStatus walk_feed(JeefsWalk *w, const uint8_t *raw) {
  const auto *file = reinterpret_cast<const JeefsFileHeader *>(raw);
  if (w->status != WalkStatus::WANT_HEADER) {
    return w->status;
  }
  // Spec: an unwritten slot reads 0x00 or 0xFF and ends the chain, never signals corruption.
  if (raw[0] == EMPTY_BYTE || raw[0] == ERASED_BYTE) {
    return w->status = WalkStatus::NOT_FOUND;
  }
  if (compute_crc32(raw, FILE_HEADER_CRC_DATA_SIZE) != file->header_crc32) {
    return w->status = WalkStatus::BAD_FILE_HEADER_CRC;
  }
  // A name that fills the field leaves no terminator, so no reader could tell where it ends.
  if (raw[FILE_NAME_SIZE - 1] != '\0' || file->data_size == 0 || file->data_size == NEXT_TERMINAL) {
    return w->status = WalkStatus::CORRUPT_CHAIN;
  }
  uint32_t end = w->want_offset + FILE_HEADER_SIZE + file->data_size;
  if (end > w->image_size) {
    return w->status = WalkStatus::CORRUPT_CHAIN;
  }
  uint16_t next = file->next_file_address == NEXT_TERMINAL ? 0 : file->next_file_address;
  if (next != 0 && (next != end || end + FILE_HEADER_SIZE > w->image_size)) {
    return w->status = WalkStatus::CORRUPT_CHAIN;
  }
  if (strncmp(file->name, w->target, FILE_NAME_SIZE) == 0) {
    w->file_offset = w->want_offset + FILE_HEADER_SIZE;
    w->file_size = file->data_size;
    w->file_crc32 = file->crc32;
    return w->status = WalkStatus::FOUND;
  }
  if (next == 0) {
    return w->status = WalkStatus::NOT_FOUND;
  }
  w->want_offset = next;
  return w->status;
}

// The file CRC gates the payload; the record's own CRC is a deliberate second check.
inline RecordStatus validate_record(const uint8_t *raw, uint32_t file_data_crc) {
  const auto *rec = reinterpret_cast<const JetHomeDeviceIdentity *>(raw);
  if (compute_crc32(raw, DEVICE_ID_SIZE) != file_data_crc) {
    return RecordStatus::BAD_FILE_DATA_CRC;
  }
  if (memcmp(rec->magic, DEVID_MAGIC, sizeof(DEVID_MAGIC)) != 0) {
    return RecordStatus::BAD_MAGIC;
  }
  if (rec->record_version != DEVID_RECORD_VERSION) {
    return RecordStatus::UNSUPPORTED_VERSION;
  }
  // Spec: an algorithm outside the enum is a parse error, never something to guess past.
  if (rec->signature_version > SIG_SECP256R1) {
    return RecordStatus::UNSUPPORTED_SIGNATURE;
  }
  if (compute_crc32(raw, DEVICE_ID_CRC_DATA_SIZE) != rec->crc32) {
    return RecordStatus::BAD_RECORD_CRC;
  }
  return RecordStatus::OK;
}

// Bounded strings may fill the field with no NUL, so the length is capped, not searched for.
inline std::string field_to_string(const char *field, size_t max_len) {
  return std::string(field, strnlen(field, max_len));
}

inline std::string format_mac(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

inline std::string format_mac_plain(const uint8_t *mac) {
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

// The canonical string the production HSM signs; a change here invalidates every device.
inline std::string signing_payload(const std::string &cpuid, const std::string &mac_plain, const std::string &usid) {
  return cpuid + ":" + mac_plain + ":" + usid;
}

// The signature covers the USID, and the USID carries the serial's last 10 digits — so a
// record serial that disagrees with it is not the one production signed.
inline bool usid_encodes_serial(const std::string &usid, const std::string &device_serial) {
  if (usid.size() != USID_V2_LENGTH) {
    return false;
  }
  std::string digits;
  for (char c : device_serial) {
    if (isdigit(static_cast<unsigned char>(c)) != 0) {
      digits.push_back(c);
    }
  }
  if (digits.empty()) {
    return false;
  }
  if (digits.size() > USID_SERIAL_LENGTH) {
    digits.erase(0, digits.size() - USID_SERIAL_LENGTH);
  }
  digits.insert(0, USID_SERIAL_LENGTH - digits.size(), '0');
  return usid.compare(USID_SERIAL_OFFSET, USID_SERIAL_LENGTH, digits) == 0;
}

}  // namespace esphome::jethome_board_info
