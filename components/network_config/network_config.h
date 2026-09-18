#pragma once

#include "esphome/core/defines.h"
#ifdef USE_NETWORK_CONFIG

#include <string>
#include "esphome/core/component.h"
#include "network_settings_json.h"
#ifdef USE_SELECT
#include "esphome/components/select/select.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_ETHERNET
#include "esphome/components/ethernet/ethernet_component.h"
#endif

namespace esphome::network_config {

// The two keys WiFiComponent::start() may pick for its credential record (wifi_component.cpp:652):
// the fixed one it uses while no station is configured, and the config-version one it moves to
// once a list is installed.
constexpr uint32_t INBOX_LEGACY_KEY = 88491487UL;
uint32_t inbox_config_key();

// wifi::SavedWifiSettings, redeclared so the slot can be read on a build without WiFi;
// network_config.cpp asserts the two layouts against each other.
struct InboxRecord {
  char ssid[33];
  char password[65];
} PACKED;  // NOLINT

// Puts the stored network settings into the interfaces and owns what the mode select and the
// device API do to them.
class NetworkConfig : public Component {
 public:
  explicit NetworkConfig(NetworkSettingsJson *settings);

  // Between the config_json keeper (HARDWARE + 5) and both interfaces (WIFI and ETHERNET are
  // 250): Ethernet's manual IP has to be set before its setup reaches start_connect_(), and the
  // WiFi list before anything enables the radio.
  float get_setup_priority() const override { return setup_priority::WIFI + 10.0f; }
  void setup() override;
  void dump_config() override;

  static constexpr uint32_t INBOX_POLL_MS = 5000;

#ifdef USE_SELECT
  void set_mode_select(select::Select *mode_select) { this->mode_select_ = mode_select; }
#endif
#ifdef USE_WIFI
  void set_wifi(wifi::WiFiComponent *wifi) { this->wifi_ = wifi; }
#endif
#ifdef USE_ETHERNET
  void set_ethernet(ethernet::EthernetComponent *ethernet) { this->ethernet_ = ethernet; }
#endif

  NetworkSettingsJson *settings() const { return this->settings_; }

  // Runs the stored mode: enables and disables the interfaces and reinstalls the WiFi network
  // list. What the "Network mode" select does, and what the boot script calls.
  void apply_mode();
  // The select's option order is Auto, Ethernet, WiFi. Stores the mode, saves it, and applies
  // it when the interfaces are already running.
  void set_network_type_from_select(size_t index);

  // The stored settings are no longer what the interfaces are running.
  bool reboot_required() const;

  // --- the device API; both go straight to the record, so the routes validate nothing ---
  const char *update_from_json(JsonObject body);
  void write_public_json(JsonObject root) const { this->settings_->write_public_json(root); }

 protected:
  // ESPHome's own WiFi preference is an inbox, not a store: read whatever the captive portal
  // left there into the record, then make the slot unreadable so start() cannot overwrite the
  // list with it. Runs at setup and on the poll.
  void drain_inbox_();
  void install_sta_();
  void apply_ethernet_ip_();
  void publish_mode_();
  void save_soon_();
  void snapshot_live_();
  void snapshot_all_();

  NetworkSettingsJson *settings_;
  std::string applied_live_;
  std::string applied_boot_;
  bool applied_{false};
#ifdef USE_SELECT
  select::Select *mode_select_{nullptr};
#endif
#ifdef USE_WIFI
  wifi::WiFiComponent *wifi_{nullptr};
#endif
#ifdef USE_ETHERNET
  ethernet::EthernetComponent *ethernet_{nullptr};
#endif
};

extern NetworkConfig *global_network_config;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::network_config

#endif  // USE_NETWORK_CONFIG
