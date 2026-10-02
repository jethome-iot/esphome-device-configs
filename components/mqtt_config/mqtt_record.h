#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include "esphome/components/json/json_util.h"
#include "esphome/core/optional.h"

namespace esphome::mqtt_config {

static constexpr size_t BROKER_MAX = 128;
static constexpr size_t USERNAME_MAX = 64;
static constexpr size_t PASSWORD_MAX = 128;
static constexpr size_t CLIENT_ID_MAX = 64;
static constexpr size_t TOPIC_PREFIX_MAX = 64;  // upstream's topic buffers assume it (mqtt_component.h)
static constexpr uint16_t DEFAULT_PORT = 1883;

// The layout this firmware writes, and the newest it applies.
static constexpr uint8_t RECORD_LAYOUT = 1;

static constexpr uint8_t FLAG_ENABLED = 1u << 0;
static constexpr uint8_t FLAG_DISCOVERY = 1u << 1;
static constexpr uint8_t FLAG_CLEAN_PENDING = 1u << 2;
static constexpr uint8_t KNOWN_FLAGS = FLAG_ENABLED | FLAG_DISCOVERY | FLAG_CLEAN_PENDING;

// One NVS record, 512 bytes for good: a preference of another size does not load, so a later
// layout grows into the reserved bytes and the free flag bits instead.
struct StoredMqttV1 {
  uint8_t layout;      // the layout that wrote it
  uint8_t min_reader;  // the oldest layout allowed to apply it
  uint8_t flags;
  uint8_t reserved8;
  uint16_t port;
  uint16_t reserved16;
  char broker[BROKER_MAX + 1];
  char username[USERNAME_MAX + 1];
  char password[PASSWORD_MAX + 1];
  char client_id[CLIENT_ID_MAX + 1];
  char topic_prefix[TOPIC_PREFIX_MAX + 1];
  uint8_t reserved[51];
};
static_assert(sizeof(StoredMqttV1) == 512, "the record never changes size");
static_assert(offsetof(StoredMqttV1, broker) == 8 && offsetof(StoredMqttV1, username) == 137 &&
                  offsetof(StoredMqttV1, password) == 202 && offsetof(StoredMqttV1, client_id) == 331 &&
                  offsetof(StoredMqttV1, topic_prefix) == 396 && offsetof(StoredMqttV1, reserved) == 461,
              "no padding: the bytes are the format");

// The settings in RAM. An empty client_id or topic_prefix means the stock default.
struct MqttRecord {
  bool enabled{false};
  bool discovery{false};
  bool clean_pending{false};   // retained discovery configs may still be on the broker
  int64_t port{DEFAULT_PORT};  // wide, so validate() sees 70000 rather than a wrapped 4464
  std::string broker;
  std::string username;
  std::string password;
  std::string client_id;
  std::string topic_prefix;

  // The fields a user sets; clean_pending is the device's own bookkeeping.
  bool same_settings(const MqttRecord &other) const;
};

// A POST body: an absent key keeps the stored value.
struct MqttPatch {
  optional<bool> enabled;
  optional<bool> discovery;
  optional<int64_t> port;
  optional<std::string> broker;
  optional<std::string> username;
  optional<std::string> password;
  optional<std::string> client_id;
  optional<std::string> topic_prefix;

  void apply_to(MqttRecord &record) const;
};

// The first rule the record breaks, nullptr when it breaks none. `ipv6` is what the build was
// compiled with: an address of two colons or more is then a host, not a host and a port.
const char *validate(const MqttRecord &record, bool ipv6);

// Reads a POST body that is already a JSON object. Types only, in the body's key order; the
// first bad key is the answer, "" when the body parsed. validate() judges the values.
std::string parse_patch(JsonObjectConst body, MqttPatch &out);

// The settings a running client compares on, with the two defaults filled in.
bool same_running_settings(const MqttRecord &a, const MqttRecord &b, const std::string &default_client_id,
                           const std::string &default_prefix);

// `base` is the record as loaded: its unknown flag bits and reserved bytes go back unchanged.
// A field longer than its buffer is cut; only a record validate() refused can have one.
StoredMqttV1 to_stored(const MqttRecord &record, const StoredMqttV1 &base);
// Every field is cut at its buffer's end, so a corrupted record still reads as text.
MqttRecord from_stored(const StoredMqttV1 &stored);

// The length of the well-formed UTF-8 sequence at `p`, its code point in `cp`; 0 for a stray
// or cut sequence, an overlong form, a surrogate or a code point past U+10FFFF.
size_t utf8_sequence(const uint8_t *p, const uint8_t *end, uint32_t &cp);

// Valid UTF-8 without control characters (U+0000–U+001F, U+007F–U+009F): overlong forms,
// surrogates, code points past U+10FFFF and cut sequences all fail.
bool is_text(const std::string &value);

}  // namespace esphome::mqtt_config
