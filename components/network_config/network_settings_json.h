#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK_CONFIG

#include <string>
#include <vector>
#include "esphome/components/config_json/config_json.h"
#include "esphome/components/config_json/settings_base_json.h"

namespace esphome::network_config {

// The order the "Network mode" select lists its options in.
enum class NetworkType : uint8_t { AUTO = 0, ETHERNET = 1, WIFI = 2 };

const char *network_type_to_string(NetworkType type);
// False on an unknown name, leaving `type` untouched.
bool parse_network_type(const char *name, NetworkType &type);
bool network_type_from_index(size_t index, NetworkType &type);
size_t network_type_index(NetworkType type);

// IEEE 802.11 and WPA2 PSK limits, the same ones cv.ssid and validate_password enforce.
static constexpr size_t SSID_MAX_LEN = 32;
static constexpr size_t PASSWORD_MIN_LEN = 8;
static constexpr size_t PASSWORD_MAX_LEN = 64;
// Array order is the priority, so no entry stores one. /capabilities reports the cap.
static constexpr size_t MAX_NETWORKS = 5;

// Addresses are kept as typed, so the file round-trips what the user entered.
struct ManualIpRecord {
  std::string static_ip;
  std::string gateway;
  std::string subnet;
  std::string dns1;
  std::string dns2;

  // The three an interface cannot go static without; DNS may be left to the network.
  bool complete() const { return !this->static_ip.empty() && !this->gateway.empty() && !this->subnet.empty(); }
  void to_json(JsonObject obj) const;
};

struct InterfaceRecord {
  bool use_dhcp{true};
  ManualIpRecord manual_ip;

  void to_json(JsonObject obj) const;
  std::string fingerprint() const;
};

struct WifiNetworkRecord {
  std::string ssid;
  std::string password;
};

// /littlefs/config/network.json: the network mode, the WiFi network list and each interface's
// IP settings. Passwords are stored in the clear. A singleton document, so it derives from
// SettingsBaseJson rather than SettingsBaseJsonTyped, whose shape is a "records" array.
class NetworkSettingsJson : public config_json::SettingsBaseJson {
 public:
  static constexpr const char *TAG = "network_config";
  static constexpr const char *NAME = "network";

  const char *get_key() override { return NAME; }

  bool parse_json(JsonObject root, uint32_t version) override;
  void write_json(JsonObject root, uint32_t version) override;
  // NetworkConfig applies the record from its own setup(), at a priority that lands between
  // the interfaces and the keeper; the keeper's apply hook is not wired for this type.
  void apply() override {}
  size_t size() override { return this->networks_.size(); }
  void reset() override;

  // --- the REST surface; the routes validate nothing of their own ---

  // The saved configuration as a client may see it: has_password, never the password.
  void write_public_json(JsonObject root) const;
  // A partial update: absent keys are unchanged, `wifi_networks` replaces the whole list, and
  // an entry with no `password` keeps the one stored for that SSID. All or nothing - on a
  // refusal the record is untouched. Returns why it was refused, or nullptr.
  const char *update_from_json(JsonObject body);

  // --- the record ---

  NetworkType network_type() const { return this->network_type_; }
  // False when the record already held that type.
  bool set_network_type(NetworkType type);
  const std::vector<WifiNetworkRecord> &networks() const { return this->networks_; }
  const InterfaceRecord &wifi() const { return this->wifi_; }
  const InterfaceRecord &ethernet() const { return this->ethernet_; }

  // Takes over credentials found in ESPHome's own WiFi preference: a new SSID becomes the
  // primary entry, a known one moves up and takes the new password. True when that changed
  // the record.
  bool adopt_network(const std::string &ssid, const std::string &password);

  // The stored document could not be understood. Nothing writes over it by itself while this
  // holds; only an explicit update clears it.
  bool is_broken() const { return this->broken_; }

  // What apply_mode() puts into effect, against what only the next boot can: ESPHome applies a
  // manual IP on (re)connect and has no way back to DHCP at run time.
  std::string live_fingerprint() const;
  std::string boot_fingerprint() const;

  // Which interfaces this firmware has, defaulted from the build. The host suite sets both, so
  // the interface half of the record is reachable without a radio.
  void set_available_interfaces(bool wifi, bool ethernet) {
    this->wifi_available_ = wifi;
    this->ethernet_available_ = ethernet;
  }
  bool wifi_available() const { return this->wifi_available_; }
  bool ethernet_available() const { return this->ethernet_available_; }

 protected:
  // Names, messages and the member each manual_ip field is read into.
  struct ManualIpField {
    const char *key;
    std::string ManualIpRecord::*member;
    const char *error;
  };
  static const ManualIpField MANUAL_IP_FIELDS[5];

  struct InterfaceMessages {
    const char *key;
    const char *not_an_object;
    const char *unavailable;
  };
  static const InterfaceMessages WIFI_MESSAGES;
  static const InterfaceMessages ETHERNET_MESSAGES;

  void set_defaults_();
  // A bad block in the file is warned about and dropped, never refused: refusing the whole
  // file would strand the device on settings it cannot edit.
  void read_interface_(JsonObject root, const char *name, InterfaceRecord &out);
  const char *merge_interface_(JsonObject body, const InterfaceMessages &messages, bool available,
                               InterfaceRecord &out) const;
  std::string password_for_(const std::string &ssid) const;

  NetworkType network_type_{NetworkType::ETHERNET};
  InterfaceRecord wifi_;
  InterfaceRecord ethernet_;
  std::vector<WifiNetworkRecord> networks_;
  bool broken_{false};
#ifdef USE_WIFI
  bool wifi_available_{true};
#else
  bool wifi_available_{false};
#endif
#ifdef USE_ETHERNET
  bool ethernet_available_{true};
#else
  bool ethernet_available_{false};
#endif
};

}  // namespace esphome::network_config

#endif  // USE_NETWORK_CONFIG
