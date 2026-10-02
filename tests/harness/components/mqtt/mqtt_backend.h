#pragma once

// Host stand-in for upstream's mqtt_backend.h: the two types the client and its users share.
// There is no backend: the stand-in client is driven by the tests instead of a socket.

#include "esphome/core/defines.h"

#ifdef USE_MQTT

#include <cstdint>
#include <string>

namespace esphome::mqtt {

enum class MQTTClientDisconnectReason : int8_t {
  TCP_DISCONNECTED = 0,
  MQTT_UNACCEPTABLE_PROTOCOL_VERSION = 1,
  MQTT_IDENTIFIER_REJECTED = 2,
  MQTT_SERVER_UNAVAILABLE = 3,
  MQTT_MALFORMED_CREDENTIALS = 4,
  MQTT_NOT_AUTHORIZED = 5,
  ESP8266_NOT_ENOUGH_SPACE = 6,
  TLS_BAD_FINGERPRINT = 7,
  DNS_RESOLVE_ERROR = 8
};

struct MQTTMessage {
  std::string topic;
  std::string payload;
  uint8_t qos;
  bool retain;
};

}  // namespace esphome::mqtt

#endif  // USE_MQTT
