#include "mqtt_record.h"
#include <algorithm>
#include <cstring>

namespace esphome::mqtt_config {

bool MqttRecord::same_settings(const MqttRecord &other) const {
  return this->enabled == other.enabled && this->discovery == other.discovery && this->port == other.port &&
         this->broker == other.broker && this->username == other.username && this->password == other.password &&
         this->client_id == other.client_id && this->topic_prefix == other.topic_prefix;
}

void MqttPatch::apply_to(MqttRecord &record) const {
  if (this->enabled.has_value())
    record.enabled = *this->enabled;
  if (this->discovery.has_value())
    record.discovery = *this->discovery;
  if (this->port.has_value())
    record.port = *this->port;
  if (this->broker.has_value())
    record.broker = *this->broker;
  if (this->username.has_value())
    record.username = *this->username;
  if (this->password.has_value())
    record.password = *this->password;
  if (this->client_id.has_value())
    record.client_id = *this->client_id;
  if (this->topic_prefix.has_value())
    record.topic_prefix = *this->topic_prefix;
}

bool is_text(const std::string &value) {
  const auto *p = reinterpret_cast<const uint8_t *>(value.data());
  const auto *end = p + value.size();
  while (p < end) {
    const uint8_t lead = *p;
    uint32_t cp;
    size_t extra;
    if (lead < 0x80) {
      cp = lead;
      extra = 0;
    } else if (lead >= 0xC2 && lead <= 0xDF) {
      cp = lead & 0x1F;
      extra = 1;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
      cp = lead & 0x0F;
      extra = 2;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
      cp = lead & 0x07;
      extra = 3;
    } else {
      return false;  // a stray continuation byte, an overlong lead (C0, C1) or past F4
    }
    if (static_cast<size_t>(end - p) <= extra)
      return false;
    for (size_t i = 1; i <= extra; i++) {
      if ((p[i] & 0xC0) != 0x80)
        return false;
      cp = (cp << 6) | (p[i] & 0x3F);
    }
    // Overlong three- and four-byte forms, surrogates, and past U+10FFFF.
    if ((extra == 2 && cp < 0x800) || (extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF))
      return false;
    if (cp <= 0x1F || (cp >= 0x7F && cp <= 0x9F))
      return false;
    p += extra + 1;
  }
  return true;
}

// Letters, digits, '.', '_' and '-': a host name or an IPv4 address; with IPv6 also ':'.
static bool is_host_char(char c, bool ipv6) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
         c == '-' || (ipv6 && c == ':');
}

const char *validate(const MqttRecord &record, bool ipv6) {
  const std::string &broker = record.broker;
  if (broker.empty()) {
    if (record.enabled)
      return "'broker' is required to turn MQTT on";
  } else {
    if (broker.size() > BROKER_MAX)
      return "'broker' is over 128 characters";
    if (broker.find("://") != std::string::npos)
      return "'broker' takes a host name, not a URL: drop the 'mqtt://'";
    // With IPv6 an address has two colons or more, so only a lone one is a port.
    const auto colons = std::count(broker.begin(), broker.end(), ':');
    if (ipv6 ? colons == 1 : colons > 0)
      return "'broker' cannot carry a port: put it in 'port'";
    if (!std::all_of(broker.begin(), broker.end(), [ipv6](char c) { return is_host_char(c, ipv6); }))
      return "'broker' must be a host name or an IPv4 address";
  }

  if (record.port < 1 || record.port > 65535)
    return "'port' must be between 1 and 65535";

  if (record.username.size() > USERNAME_MAX)
    return "'username' is over 64 bytes";
  if (!is_text(record.username))
    return "'username' must be text without control characters";

  if (record.password.size() > PASSWORD_MAX)
    return "'password' is over 128 bytes";
  if (record.password.find('\0') != std::string::npos)
    return "'password' cannot contain a NUL character";
  if (!record.password.empty() && record.username.empty())
    return "'password' needs a 'username': MQTT 3.1.1 sends none without one";

  if (record.client_id.size() > CLIENT_ID_MAX)
    return "'client_id' is over 64 characters";
  if (!std::all_of(record.client_id.begin(), record.client_id.end(), [](char c) { return c >= 0x21 && c <= 0x7E; }))
    return "'client_id' must be printable ASCII without spaces";

  const std::string &prefix = record.topic_prefix;
  if (prefix.size() > TOPIC_PREFIX_MAX)
    return "'topic_prefix' is over 64 bytes";
  if (prefix.find_first_of("+#") != std::string::npos)
    return "'topic_prefix' cannot contain '+' or '#'";
  if (!is_text(prefix))
    return "'topic_prefix' must be text without control characters";
  if (!prefix.empty() && prefix.front() == '$')
    return "'topic_prefix' cannot start with '$'";
  if (!prefix.empty() && prefix.back() == '/')
    return "'topic_prefix' cannot end with '/'";
  return nullptr;
}

std::string parse_patch(JsonObjectConst body, MqttPatch &out) {
  for (JsonPairConst kv : body) {
    const std::string key(kv.key().c_str(), kv.key().size());
    JsonVariantConst value = kv.value();
    if (key == "enabled" || key == "discovery") {
      if (!value.is<bool>())
        return "'" + key + "' must be true or false";
      (key == "enabled" ? out.enabled : out.discovery) = value.as<bool>();
    } else if (key == "port") {
      // int64_t, so a whole number out of range parses and validate() answers with the range,
      // where a narrower read would wrap it or call it not a number.
      if (!value.is<int64_t>())
        return "'port' must be a whole number";
      out.port = value.as<int64_t>();
    } else if (key == "broker" || key == "username" || key == "password" || key == "client_id" ||
               key == "topic_prefix") {
      if (!value.is<const char *>())
        return "'" + key + "' must be a string";
      // Straight to std::string: through a C string a NUL would end the value early, and
      // validate() is what refuses it.
      std::string text = value.as<std::string>();
      if (key == "broker") {
        out.broker = std::move(text);
      } else if (key == "username") {
        out.username = std::move(text);
      } else if (key == "password") {
        out.password = std::move(text);
      } else if (key == "client_id") {
        out.client_id = std::move(text);
      } else {
        out.topic_prefix = std::move(text);
      }
    } else {
      return "'" + key + "' is not an MQTT setting";
    }
  }
  return "";
}

static const std::string &or_default(const std::string &value, const std::string &fallback) {
  return value.empty() ? fallback : value;
}

bool same_running_settings(const MqttRecord &a, const MqttRecord &b, const std::string &default_client_id,
                           const std::string &default_prefix) {
  return a.enabled == b.enabled && a.broker == b.broker && a.port == b.port && a.username == b.username &&
         a.password == b.password &&
         or_default(a.client_id, default_client_id) == or_default(b.client_id, default_client_id) &&
         or_default(a.topic_prefix, default_prefix) == or_default(b.topic_prefix, default_prefix) &&
         a.discovery == b.discovery;
}

template<size_t N> static void copy_field(char (&dst)[N], const std::string &src) {
  std::memset(dst, 0, N);
  std::memcpy(dst, src.data(), std::min(src.size(), N - 1));
}

template<size_t N> static std::string read_field(const char (&src)[N]) { return std::string(src, strnlen(src, N - 1)); }

StoredMqttV1 to_stored(const MqttRecord &record, const StoredMqttV1 &base) {
  StoredMqttV1 out = base;
  // This firmware wrote it last, so a newer reader knows the reserved bytes may be stale.
  out.layout = RECORD_LAYOUT;
  out.min_reader = RECORD_LAYOUT;
  out.flags =
      static_cast<uint8_t>((base.flags & ~KNOWN_FLAGS) | (record.enabled ? FLAG_ENABLED : 0) |
                           (record.discovery ? FLAG_DISCOVERY : 0) | (record.clean_pending ? FLAG_CLEAN_PENDING : 0));
  out.port = static_cast<uint16_t>(record.port);
  copy_field(out.broker, record.broker);
  copy_field(out.username, record.username);
  copy_field(out.password, record.password);
  copy_field(out.client_id, record.client_id);
  copy_field(out.topic_prefix, record.topic_prefix);
  return out;
}

MqttRecord from_stored(const StoredMqttV1 &stored) {
  MqttRecord record;
  record.enabled = (stored.flags & FLAG_ENABLED) != 0;
  record.discovery = (stored.flags & FLAG_DISCOVERY) != 0;
  record.clean_pending = (stored.flags & FLAG_CLEAN_PENDING) != 0;
  record.port = stored.port;
  record.broker = read_field(stored.broker);
  record.username = read_field(stored.username);
  record.password = read_field(stored.password);
  record.client_id = read_field(stored.client_id);
  record.topic_prefix = read_field(stored.topic_prefix);
  return record;
}

}  // namespace esphome::mqtt_config
