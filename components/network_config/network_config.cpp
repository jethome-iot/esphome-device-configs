#include "network_config.h"
#ifdef USE_NETWORK_CONFIG

#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "ip4.h"

namespace esphome::network_config {

NetworkConfig *global_network_config = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

static const char *const TAG = "network_config";

#if defined(USE_WIFI) || defined(USE_ETHERNET)
namespace {

network::IPAddress to_ip(uint32_t address) {
  return network::IPAddress(static_cast<uint8_t>((address >> 24) & 0xFF), static_cast<uint8_t>((address >> 16) & 0xFF),
                            static_cast<uint8_t>((address >> 8) & 0xFF), static_cast<uint8_t>(address & 0xFF));
}

// wifi::ManualIP and ethernet::ManualIP are the same five IPv4 addresses. The strings have been
// through parse_ip4() already, so the octets go in directly rather than through the aton() that
// has no way to report a bad string (network/ip_address.h:118-121, :203-204).
template<typename ManualIpT> bool build_manual_ip(const ManualIpRecord &record, ManualIpT &out) {
  uint32_t address = 0;
  if (!parse_ip4(record.static_ip, address))
    return false;
  out.static_ip = to_ip(address);
  if (!parse_ip4(record.gateway, address))
    return false;
  out.gateway = to_ip(address);
  if (!parse_ip4(record.subnet, address))
    return false;
  out.subnet = to_ip(address);
  // 0.0.0.0 is what both components read as "no DNS server of our own".
  out.dns1 = parse_ip4(record.dns1, address) ? to_ip(address) : network::IPAddress();
  out.dns2 = parse_ip4(record.dns2, address) ? to_ip(address) : network::IPAddress();
  return true;
}

}  // namespace
#endif

#ifdef USE_WIFI
static_assert(sizeof(InboxRecord) == sizeof(wifi::SavedWifiSettings),
              "wifi::SavedWifiSettings changed shape; the inbox layout has to follow");
#endif

namespace {

// Written over a drained key. load() fills its destination only when the stored length matches
// (core/preference_backend.h:35-41), so wifi's own load returns false and start() finds nothing
// to set_sta() the list away with.
struct InboxPoison {
  uint32_t marker;
} PACKED;  // NOLINT

static_assert(sizeof(InboxPoison) != sizeof(InboxRecord), "the poison must not read as a record");

constexpr uint32_t POISON_MARKER = 0x4E434647UL;  // 'NCFG', so a hex dump names the writer

}  // namespace

uint32_t inbox_config_key() { return App.get_config_version_hash(); }

NetworkConfig::NetworkConfig(NetworkSettingsJson *settings) : settings_(settings) { global_network_config = this; }

void NetworkConfig::setup() {
  this->drain_inbox_();
  this->apply_ethernet_ip_();
  this->install_sta_();
  this->publish_mode_();
  this->snapshot_all_();
#ifdef USE_PREFERENCE_KEY_LOOKUP
  // Polled because save_wifi_sta() has no hook and an update before the next boot moves the key.
  this->set_interval("inbox", INBOX_POLL_MS, [this]() { this->drain_inbox_(); });
#endif
}

void NetworkConfig::dump_config() {
  ESP_LOGCONFIG(
      TAG,
      "Network Config:\n"
      "  Mode: %s\n"
      "  WiFi networks: %u\n"
      "  WiFi IP: %s\n"
      "  Ethernet IP: %s",
      network_type_to_string(this->settings_->network_type()),
      static_cast<unsigned>(this->settings_->networks().size()),
      this->settings_->wifi().use_dhcp ? "DHCP" : this->settings_->wifi().manual_ip.static_ip.c_str(),
      this->settings_->ethernet().use_dhcp ? "DHCP" : this->settings_->ethernet().manual_ip.static_ip.c_str());
  if (this->settings_->is_broken())
    ESP_LOGW(TAG, "The stored network settings could not be read; nothing is saved until they are replaced");
}

void NetworkConfig::drain_inbox_() {
#ifdef USE_PREFERENCE_KEY_LOOKUP
  // Nothing is touched while the record is unreadable: the slot may hold the only copy.
  if (this->settings_->is_broken())
    return;
  const uint32_t keys[] = {INBOX_LEGACY_KEY, inbox_config_key()};
  uint32_t filled[sizeof(keys) / sizeof(keys[0])];
  size_t filled_count = 0;
  bool adopted = false;
  for (const uint32_t key : keys) {
    InboxRecord record{};
    if (!global_preferences->load_from_key(key, reinterpret_cast<uint8_t *>(&record), sizeof(record)))
      continue;
    filled[filled_count++] = key;
    record.ssid[sizeof(record.ssid) - 1] = '\0';
    record.password[sizeof(record.password) - 1] = '\0';
    // An empty SSID is refused by adopt_network(): upstream matches it against any open AP.
    if (this->settings_->adopt_network(record.ssid, record.password)) {
      adopted = true;
      ESP_LOGI(TAG, "Took over the credentials for '%s'", record.ssid);
    }
  }
  // Only a slot that reads as a record is written over: the poll finds nothing on every other pass.
  if (filled_count == 0)
    return;

  if (this->settings_->is_dirty() && config_json::global_config_json_keeper != nullptr)
    config_json::global_config_json_keeper->save_immediate(NetworkSettingsJson::NAME);
  if (this->settings_->is_dirty()) {
    if (adopted)
      ESP_LOGE(TAG, "The adopted credentials are not on flash yet; leaving the WiFi preference alone");
    return;
  }

  InboxPoison poison{POISON_MARKER};
  for (size_t i = 0; i < filled_count; i++) {
    auto pref = global_preferences->make_preference<InboxPoison>(filled[i], true);
    pref.save(&poison);
  }
  global_preferences->sync();
  ESP_LOGI(TAG, "Cleared ESPHome's WiFi credential slot");
#endif
}

void NetworkConfig::apply_ethernet_ip_() {
#ifdef USE_ETHERNET
  if (this->ethernet_ == nullptr)
    return;
  const InterfaceRecord &record = this->settings_->ethernet();
  if (record.use_dhcp)
    return;  // the compiled default; there is no runtime way back to DHCP either
  ethernet::ManualIP manual;
  if (!build_manual_ip(record.manual_ip, manual)) {
    ESP_LOGE(TAG, "The stored Ethernet address is incomplete; staying on DHCP");
    return;
  }
  this->ethernet_->set_manual_ip(manual);
  ESP_LOGI(TAG, "Ethernet runs on %s", record.manual_ip.static_ip.c_str());
#endif
}

void NetworkConfig::install_sta_() {
#ifdef USE_WIFI
  if (this->wifi_ == nullptr)
    return;
  const auto &networks = this->settings_->networks();
  // With nothing stored, nothing is installed: start() then raises the provisioning AP at once,
  // and an already running component keeps the list it connected with.
  if (networks.empty())
    return;

  const InterfaceRecord &ip = this->settings_->wifi();
#ifdef USE_WIFI_MANUAL_IP
  wifi::ManualIP manual;
  const bool has_manual_ip = !ip.use_dhcp && build_manual_ip(ip.manual_ip, manual);
  if (!ip.use_dhcp && !has_manual_ip)
    ESP_LOGE(TAG, "The stored WiFi address is incomplete; staying on DHCP");
#endif

  // sta_ is a FixedVector sized by codegen, which emits init_sta() only for a YAML that lists
  // networks: (wifi/__init__.py:618-620). Its capacity is 0 here, and push_back past it is a
  // silent no-op, so every install starts with init_sta().
  this->wifi_->init_sta(networks.size());
  for (size_t i = 0; i < networks.size(); i++) {
    wifi::WiFiAP ap;
    ap.set_ssid(networks[i].ssid);
    ap.set_password(networks[i].password);
    // Array order is the priority, and upstream sorts descending.
    ap.set_priority(static_cast<int8_t>(networks.size() - 1 - i));
#ifdef USE_WIFI_MANUAL_IP
    if (has_manual_ip)
      ap.set_manual_ip(manual);
#endif
    this->wifi_->add_sta(ap);
  }
  ESP_LOGI(TAG, "Installed %u WiFi network(s)", static_cast<unsigned>(networks.size()));
#endif
}

void NetworkConfig::publish_mode_() {
#ifdef USE_SELECT
  if (this->mode_select_ == nullptr)
    return;
  const size_t index = network_type_index(this->settings_->network_type());
  if (this->mode_select_->has_index(index))
    this->mode_select_->publish_state(index);
#endif
}

void NetworkConfig::apply_mode() {
  const NetworkType type = this->settings_->network_type();
  ESP_LOGD(TAG, "Applying network mode '%s'", network_type_to_string(type));
#ifdef USE_ETHERNET
  if (this->ethernet_ != nullptr) {
    if (type == NetworkType::WIFI) {
      this->ethernet_->disable();
    } else {
      this->ethernet_->enable();
    }
  }
#endif
#ifdef USE_WIFI
  if (this->wifi_ != nullptr) {
    if (type == NetworkType::ETHERNET) {
      this->wifi_->disable();
    } else {
      // enable() runs start() synchronously, and start() replaces the whole station list with
      // whatever its preference holds (wifi_component.cpp:665-672, :1034). Reinstalling right
      // after it returns closes that window with no loop tick in between.
      this->wifi_->enable();
      this->install_sta_();
    }
  }
#endif
  this->applied_ = true;
  this->snapshot_live_();
}

void NetworkConfig::set_network_type_from_select(size_t index) {
  NetworkType type = NetworkType::ETHERNET;
  if (!network_type_from_index(index, type)) {
    ESP_LOGW(TAG, "The mode select offered index %u, which is no network mode", static_cast<unsigned>(index));
    return;
  }
  if (!this->settings_->set_network_type(type))
    return;
  this->save_soon_();
  // Applied right away, and the snapshot moves with it, so a mode chosen on the display does
  // not raise a reboot banner for a change that already took effect.
  if (this->applied_)
    this->apply_mode();
}

bool NetworkConfig::reboot_required() const {
  return this->settings_->live_fingerprint() != this->applied_live_ ||
         this->settings_->boot_fingerprint() != this->applied_boot_;
}

const char *NetworkConfig::update_from_json(JsonObject body) {
  const char *error = this->settings_->update_from_json(body);
  if (error != nullptr)
    return error;
  this->save_soon_();
  return nullptr;
}

void NetworkConfig::save_soon_() {
  // The device API calls this from the server task, and the write has to run on the loop.
  // Named, so a burst of requests ends in one write; an unnamed defer never coalesces.
  this->defer("save", []() {
    if (config_json::global_config_json_keeper != nullptr)
      config_json::global_config_json_keeper->save_immediate(NetworkSettingsJson::NAME);
  });
}

void NetworkConfig::snapshot_live_() { this->applied_live_ = this->settings_->live_fingerprint(); }

void NetworkConfig::snapshot_all_() {
  this->snapshot_live_();
  this->applied_boot_ = this->settings_->boot_fingerprint();
}

}  // namespace esphome::network_config

#endif  // USE_NETWORK_CONFIG
