#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "esphome/components/json/json_util.h"

namespace esphome::mqtt_subscriptions {

static constexpr uint8_t MAX_SLOTS = 16;
static constexpr size_t SLOT_NAME_MAX = 32;
static constexpr size_t TOPIC_MAX = 128;
static constexpr size_t JSON_PATH_MAX = 64;
static constexpr size_t JSON_PATH_SEGMENTS = 6;
static constexpr size_t PAYLOAD_STATE_MAX = 32;
static constexpr int64_t DECIMALS_MAX = 4;
static constexpr int64_t DEFAULT_DECIMALS = 1;

// The file layout this firmware writes, and the newest it reads.
static constexpr int64_t FILE_VERSION = 1;
static constexpr size_t FILE_MAX = 16 * 1024;

// Domain names: the dashboard joins slots to entities on them.
enum class SlotKind : uint8_t { SENSOR, BINARY_SENSOR, TEXT_SENSOR };

const char *kind_key(SlotKind kind);

// A JSON document that allocates from PSRAM first where the board has it.
struct Document {
#ifdef USE_PSRAM
  json::SpiRamAllocator allocator;
  JsonDocument doc{&allocator};
#else
  JsonDocument doc;
#endif
};

// One slot as saved. An empty topic is an empty slot.
struct SlotConfig {
  bool enabled{false};
  std::string name;
  std::string topic;
  SlotKind kind{SlotKind::SENSOR};
  std::string json_path;
  std::string unit;
  int64_t decimals{DEFAULT_DECIMALS};  // wide, so 260 is refused rather than wrapped to 4
  std::string payload_on{"ON"};
  std::string payload_off{"OFF"};

  bool empty() const { return this->topic.empty(); }
  // Fields the kind does not use go back to their defaults; an empty slot is all defaults.
  void normalize();
  // Every field but the slot number.
  void to_json(JsonObject obj) const;

  bool operator==(const SlotConfig &other) const;
  bool operator!=(const SlotConfig &other) const { return !(*this == other); }
};

// How strictly read_slot() takes an object: a POST body or an entry of the file.
enum class Source : uint8_t { BODY, FILE };

// Types only, in the object's key order; the first bad key is the answer, "" when it read.
// The body needs `enabled` and `kind`; an unknown key is refused there and skipped in the
// file. The name is trimmed. `slot`, `pending`, `status` and `entity` are skipped.
std::string read_slot(JsonObjectConst obj, Source source, SlotConfig &out);

// The first rule of the slot alone it breaks, "" when none; the names it shares with other
// entities are the component's to check. `units` is what this firmware offers.
std::string validate_slot(const SlotConfig &slot, const std::vector<std::string> &units);

// What EntityBase makes of a name: lower case, '_' for a space and for every byte outside
// [a-z0-9_-], so two Cyrillic names of the same byte length come out alike.
std::string object_id_of(const std::string &name);

// 1 to JSON_PATH_SEGMENTS non-empty keys separated by '.'.
bool json_path_valid(const std::string &path);

// Without leading and trailing ASCII whitespace.
std::string trim_ascii(const std::string &text);
// ASCII letters compared without case, every other byte as it is.
bool equal_ignoring_case(const std::string &a, const std::string &b);

struct SlotFile {
  // UNREADABLE: read, and broken. FAILED: not read whole this time (out of memory, a short
  // read); nothing is concluded from it.
  enum class Status : uint8_t { OK, MISSING, UNREADABLE, NEWER, FAILED };
  Status status{Status::MISSING};
  std::vector<SlotConfig> slots;  // max_slots of them, the empty ones included
};

// What a JSON parse error says about the file: running out of memory is no fault of its own.
SlotFile::Status parse_failure_status(DeserializationError error);

// The file's text. Entries past `max_slots`, repeated numbers and entries of the wrong type
// are dropped with a log line; slots whose values break a rule are kept as written.
SlotFile parse_file(const char *data, size_t len, size_t max_slots);
// The non-empty slots, each with every field.
std::string serialize_file(const std::vector<SlotConfig> &slots);

}  // namespace esphome::mqtt_subscriptions
