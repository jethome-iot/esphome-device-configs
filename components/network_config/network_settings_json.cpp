#include "network_settings_json.h"
#ifdef USE_NETWORK_CONFIG

#include <cstring>
#include "esphome/core/log.h"
#include "ip4.h"

namespace esphome::network_config {

namespace {

struct NetworkTypeName {
  NetworkType type;
  const char *name;
};

// Index order is the "Network mode" select's option order: Auto, Ethernet, WiFi.
constexpr NetworkTypeName NETWORK_TYPE_NAMES[] = {
    {NetworkType::AUTO, "auto"},
    {NetworkType::ETHERNET, "ethernet"},
    {NetworkType::WIFI, "wifi"},
};

// Every refusal the update can answer with. The route sends them on as 400 and openapi lists
// them, so they are literals and none of them carries a value from the body.
const char *const ERR_INVALID_JSON = "Invalid JSON";
const char *const ERR_NETWORK_TYPE = "'network_type' must be 'auto', 'wifi' or 'ethernet'";
const char *const ERR_NETWORK_TYPE_UNAVAILABLE = "'network_type' is not available on this firmware";
const char *const ERR_NETWORKS_ARRAY = "'wifi_networks' must be an array";
const char *const ERR_NETWORKS_CAP = "'wifi_networks' holds at most 5 entries";
const char *const ERR_NETWORKS_ENTRY = "Each entry of 'wifi_networks' must be an object";
const char *const ERR_SSID = "'ssid' must be 1 to 32 characters";
const char *const ERR_PASSWORD = "'password' must be empty or 8 to 64 characters";
const char *const ERR_DUPLICATE_SSID = "Duplicate SSID in 'wifi_networks'";
const char *const ERR_USE_DHCP = "'use_dhcp' must be a boolean";
const char *const ERR_MANUAL_IP_OBJECT = "'manual_ip' must be an object";
const char *const ERR_MANUAL_IP_INCOMPLETE = "'manual_ip' needs 'static_ip', 'gateway' and 'subnet' without DHCP";

// The cap is in the message the client reads and in the one /capabilities reports.
static_assert(MAX_NETWORKS == 5, "ERR_NETWORKS_CAP names the cap");

}  // namespace

const NetworkSettingsJson::ManualIpField NetworkSettingsJson::MANUAL_IP_FIELDS[5] = {
    {"static_ip", &ManualIpRecord::static_ip, "'manual_ip.static_ip' is not an IPv4 address"},
    {"gateway", &ManualIpRecord::gateway, "'manual_ip.gateway' is not an IPv4 address"},
    {"subnet", &ManualIpRecord::subnet, "'manual_ip.subnet' is not an IPv4 address"},
    {"dns1", &ManualIpRecord::dns1, "'manual_ip.dns1' is not an IPv4 address"},
    {"dns2", &ManualIpRecord::dns2, "'manual_ip.dns2' is not an IPv4 address"},
};

const NetworkSettingsJson::InterfaceMessages NetworkSettingsJson::WIFI_MESSAGES = {"wifi", "'wifi' must be an object",
                                                                                   "This firmware has no WiFi"};
const NetworkSettingsJson::InterfaceMessages NetworkSettingsJson::ETHERNET_MESSAGES = {
    "ethernet", "'ethernet' must be an object", "This firmware has no Ethernet"};

const char *network_type_to_string(NetworkType type) {
  for (const auto &entry : NETWORK_TYPE_NAMES) {
    if (entry.type == type)
      return entry.name;
  }
  return "ethernet";
}

bool parse_network_type(const char *name, NetworkType &type) {
  if (name == nullptr)
    return false;
  for (const auto &entry : NETWORK_TYPE_NAMES) {
    if (std::strcmp(entry.name, name) == 0) {
      type = entry.type;
      return true;
    }
  }
  return false;
}

bool network_type_from_index(size_t index, NetworkType &type) {
  if (index >= sizeof(NETWORK_TYPE_NAMES) / sizeof(NETWORK_TYPE_NAMES[0]))
    return false;
  type = NETWORK_TYPE_NAMES[index].type;
  return true;
}

size_t network_type_index(NetworkType type) {
  for (size_t i = 0; i < sizeof(NETWORK_TYPE_NAMES) / sizeof(NETWORK_TYPE_NAMES[0]); i++) {
    if (NETWORK_TYPE_NAMES[i].type == type)
      return i;
  }
  return 1;  // Ethernet, the mode that needs no credentials
}

void ManualIpRecord::to_json(JsonObject obj) const {
  // Always the five keys, "" for an address that is not set: the form reads them back as text.
  obj["static_ip"] = this->static_ip;
  obj["gateway"] = this->gateway;
  obj["subnet"] = this->subnet;
  obj["dns1"] = this->dns1;
  obj["dns2"] = this->dns2;
}

void InterfaceRecord::to_json(JsonObject obj) const {
  obj["use_dhcp"] = this->use_dhcp;
  this->manual_ip.to_json(obj["manual_ip"].to<JsonObject>());
}

std::string InterfaceRecord::fingerprint() const {
  if (this->use_dhcp)
    return "dhcp";
  return this->manual_ip.static_ip + "/" + this->manual_ip.subnet + "/" + this->manual_ip.gateway + "/" +
         this->manual_ip.dns1 + "/" + this->manual_ip.dns2;
}

void NetworkSettingsJson::set_defaults_() {
  // Ethernet is what the device shipped with as the select's initial option, so an upgrade
  // without a file changes nothing for a device that never touched the mode.
  this->network_type_ = NetworkType::ETHERNET;
  this->wifi_ = InterfaceRecord{};
  this->ethernet_ = InterfaceRecord{};
  this->networks_.clear();
}

bool NetworkSettingsJson::parse_json(JsonObject root, uint32_t version) {
  this->set_defaults_();

  const bool has_type = !root["network_type"].isNull();
  const bool has_list = !root["wifi_networks"].isNull();
  if (!has_type && !has_list && root["wifi"].isNull() && root["ethernet"].isNull()) {
    ESP_LOGE(TAG, "%s.json holds none of the expected keys; keeping the defaults", NAME);
    this->broken_ = true;
    return false;
  }
  this->broken_ = false;

  if (has_type && (!root["network_type"].is<const char *>() ||
                   !parse_network_type(root["network_type"].as<const char *>(), this->network_type_))) {
    ESP_LOGW(TAG, "Unknown network_type in %s.json, using '%s'", NAME, network_type_to_string(this->network_type_));
  }

  this->read_interface_(root, "wifi", this->wifi_);
  this->read_interface_(root, "ethernet", this->ethernet_);

  if (has_list) {
    if (!root["wifi_networks"].is<JsonArray>()) {
      ESP_LOGW(TAG, "'wifi_networks' in %s.json is not an array; no WiFi network is configured", NAME);
    } else {
      // Bound to a local: iterating the member proxy directly leaves the range holding a
      // reference into a temporary.
      JsonArray array = root["wifi_networks"].as<JsonArray>();
      for (JsonVariant item : array) {
        if (this->networks_.size() >= MAX_NETWORKS) {
          ESP_LOGW(TAG, "%s.json lists more than %u WiFi networks; the rest is dropped", NAME,
                   static_cast<unsigned>(MAX_NETWORKS));
          break;
        }
        // One bad entry is dropped, so the others still reach the radio.
        if (!item.is<JsonObject>() || !item["ssid"].is<const char *>()) {
          ESP_LOGW(TAG, "Dropping a malformed WiFi network from %s.json", NAME);
          continue;
        }
        WifiNetworkRecord record{item["ssid"].as<std::string>(), item["password"] | ""};
        if (record.ssid.empty() || record.ssid.size() > SSID_MAX_LEN) {
          ESP_LOGW(TAG, "Dropping a WiFi network with an unusable SSID from %s.json", NAME);
          continue;
        }
        this->networks_.push_back(std::move(record));
      }
    }
  }

  ESP_LOGI(TAG, "Loaded network settings: mode '%s', %u WiFi network(s)", network_type_to_string(this->network_type_),
           static_cast<unsigned>(this->networks_.size()));
  return true;
}

void NetworkSettingsJson::read_interface_(JsonObject root, const char *name, InterfaceRecord &out) {
  if (root[name].isNull())
    return;
  if (!root[name].is<JsonObject>()) {
    ESP_LOGW(TAG, "'%s' in %s.json is not an object; that interface stays on DHCP", name, NAME);
    return;
  }
  JsonObject obj = root[name].as<JsonObject>();
  out.use_dhcp = obj["use_dhcp"] | true;

  JsonObject manual = obj["manual_ip"].as<JsonObject>();
  if (!manual.isNull()) {
    for (const auto &field : MANUAL_IP_FIELDS) {
      const std::string value = manual[field.key] | "";
      if (value.empty() || is_ip4(value)) {
        out.manual_ip.*field.member = value;
      } else {
        ESP_LOGW(TAG, "'%s.manual_ip.%s' in %s.json is not an IPv4 address; dropping it", name, field.key, NAME);
      }
    }
  }

  if (!out.use_dhcp && !out.manual_ip.complete()) {
    ESP_LOGW(TAG, "'%s' in %s.json has no complete static configuration; falling back to DHCP", name, NAME);
    out.use_dhcp = true;
  }
}

void NetworkSettingsJson::write_json(JsonObject root, uint32_t version) {
  root["network_type"] = network_type_to_string(this->network_type_);
  this->wifi_.to_json(root["wifi"].to<JsonObject>());
  this->ethernet_.to_json(root["ethernet"].to<JsonObject>());
  JsonArray array = root["wifi_networks"].to<JsonArray>();
  for (const auto &network : this->networks_) {
    JsonObject obj = array.add<JsonObject>();
    obj["ssid"] = network.ssid;
    obj["password"] = network.password;
  }
}

void NetworkSettingsJson::write_public_json(JsonObject root) const {
  root["network_type"] = network_type_to_string(this->network_type_);
  if (this->wifi_available_) {
    this->wifi_.to_json(root["wifi"].to<JsonObject>());
    JsonArray array = root["wifi_networks"].to<JsonArray>();
    for (const auto &network : this->networks_) {
      JsonObject obj = array.add<JsonObject>();
      obj["ssid"] = network.ssid;
      // All a client ever learns about a stored password.
      obj["has_password"] = !network.password.empty();
    }
  }
  if (this->ethernet_available_)
    this->ethernet_.to_json(root["ethernet"].to<JsonObject>());
}

void NetworkSettingsJson::reset() {
  this->set_defaults_();
  this->broken_ = false;
  this->mark_dirty();
  ESP_LOGI(TAG, "Network settings back to the compiled defaults");
}

std::string NetworkSettingsJson::password_for_(const std::string &ssid) const {
  for (const auto &network : this->networks_) {
    if (network.ssid == ssid)
      return network.password;
  }
  return "";
}

const char *NetworkSettingsJson::merge_interface_(JsonObject body, const InterfaceMessages &messages, bool available,
                                                  InterfaceRecord &out) const {
  if (body[messages.key].isNull())
    return nullptr;
  if (!available)
    return messages.unavailable;
  if (!body[messages.key].is<JsonObject>())
    return messages.not_an_object;
  JsonObject obj = body[messages.key].as<JsonObject>();

  if (!obj["use_dhcp"].isNull()) {
    if (!obj["use_dhcp"].is<bool>())
      return ERR_USE_DHCP;
    out.use_dhcp = obj["use_dhcp"].as<bool>();
  }

  if (!obj["manual_ip"].isNull()) {
    if (!obj["manual_ip"].is<JsonObject>())
      return ERR_MANUAL_IP_OBJECT;
    JsonObject manual = obj["manual_ip"].as<JsonObject>();
    // Field by field, so {"use_dhcp": true} alone does not discard the stored addresses.
    for (const auto &field : MANUAL_IP_FIELDS) {
      if (manual[field.key].isNull())
        continue;
      if (!manual[field.key].is<const char *>())
        return field.error;
      const std::string value = manual[field.key].as<std::string>();
      if (!value.empty() && !is_ip4(value))
        return field.error;
      out.manual_ip.*field.member = value;
    }
  }

  // A static interface with a hole in its addresses would come up with no address and no DHCP
  // client, so the request is refused rather than stored.
  if (!out.use_dhcp && !out.manual_ip.complete())
    return ERR_MANUAL_IP_INCOMPLETE;
  return nullptr;
}

const char *NetworkSettingsJson::update_from_json(JsonObject body) {
  if (body.isNull())
    return ERR_INVALID_JSON;

  NetworkType type = this->network_type_;
  if (!body["network_type"].isNull()) {
    if (!body["network_type"].is<const char *>() || !parse_network_type(body["network_type"].as<const char *>(), type))
      return ERR_NETWORK_TYPE;
    if ((type == NetworkType::WIFI && !this->wifi_available_) ||
        (type == NetworkType::ETHERNET && !this->ethernet_available_))
      return ERR_NETWORK_TYPE_UNAVAILABLE;
  }

  std::vector<WifiNetworkRecord> networks = this->networks_;
  if (!body["wifi_networks"].isNull()) {
    if (!this->wifi_available_)
      return WIFI_MESSAGES.unavailable;
    if (!body["wifi_networks"].is<JsonArray>())
      return ERR_NETWORKS_ARRAY;
    JsonArray array = body["wifi_networks"].as<JsonArray>();
    if (array.size() > MAX_NETWORKS)
      return ERR_NETWORKS_CAP;

    std::vector<WifiNetworkRecord> parsed;
    for (JsonVariant item : array) {
      if (!item.is<JsonObject>())
        return ERR_NETWORKS_ENTRY;
      JsonObject entry = item.as<JsonObject>();
      if (!entry["ssid"].is<const char *>())
        return ERR_SSID;
      WifiNetworkRecord record;
      record.ssid = entry["ssid"].as<std::string>();
      if (record.ssid.empty() || record.ssid.size() > SSID_MAX_LEN)
        return ERR_SSID;
      // SSIDs are octets and case matters, so "Home" and "home" are two storable networks.
      for (const auto &seen : parsed) {
        if (seen.ssid == record.ssid)
          return ERR_DUPLICATE_SSID;
      }
      if (entry["password"].isNull()) {
        // A list read back from GET carries no passwords; this is what makes it round-trip.
        record.password = this->password_for_(record.ssid);
      } else {
        if (!entry["password"].is<const char *>())
          return ERR_PASSWORD;
        record.password = entry["password"].as<std::string>();
        if (!record.password.empty() &&
            (record.password.size() < PASSWORD_MIN_LEN || record.password.size() > PASSWORD_MAX_LEN))
          return ERR_PASSWORD;
      }
      parsed.push_back(std::move(record));
    }
    networks = std::move(parsed);
  }

  InterfaceRecord wifi = this->wifi_;
  InterfaceRecord ethernet = this->ethernet_;
  if (const char *error = this->merge_interface_(body, WIFI_MESSAGES, this->wifi_available_, wifi); error != nullptr)
    return error;
  if (const char *error = this->merge_interface_(body, ETHERNET_MESSAGES, this->ethernet_available_, ethernet);
      error != nullptr)
    return error;

  // Nothing was refused, so the whole body lands at once.
  this->network_type_ = type;
  this->networks_ = std::move(networks);
  this->wifi_ = wifi;
  this->ethernet_ = ethernet;
  this->broken_ = false;
  this->mark_dirty();
  return nullptr;
}

bool NetworkSettingsJson::set_network_type(NetworkType type) {
  if (this->network_type_ == type)
    return false;
  this->network_type_ = type;
  this->mark_dirty();
  return true;
}

bool NetworkSettingsJson::adopt_network(const std::string &ssid, const std::string &password) {
  if (ssid.empty() || ssid.size() > SSID_MAX_LEN)
    return false;
  for (size_t i = 0; i < this->networks_.size(); i++) {
    if (this->networks_[i].ssid != ssid)
      continue;
    if (i == 0 && this->networks_[i].password == password)
      return false;
    WifiNetworkRecord record{ssid, password};
    this->networks_.erase(this->networks_.begin() + i);
    this->networks_.insert(this->networks_.begin(), std::move(record));
    this->mark_dirty();
    return true;
  }
  this->networks_.insert(this->networks_.begin(), WifiNetworkRecord{ssid, password});
  if (this->networks_.size() > MAX_NETWORKS)
    this->networks_.resize(MAX_NETWORKS);
  this->mark_dirty();
  return true;
}

std::string NetworkSettingsJson::live_fingerprint() const {
  std::string out = network_type_to_string(this->network_type_);
  for (const auto &network : this->networks_) {
    out += '\n';
    out += network.ssid;
    out += '\t';
    out += network.password;
  }
  return out;
}

std::string NetworkSettingsJson::boot_fingerprint() const {
  return this->wifi_.fingerprint() + "|" + this->ethernet_.fingerprint();
}

}  // namespace esphome::network_config

#endif  // USE_NETWORK_CONFIG
