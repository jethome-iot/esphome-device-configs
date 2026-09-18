#include "web_device_dashboard.h"
#include <ArduinoJson.h>
#include "dashboard_index.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/alloc_helpers.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/preferences.h"
#include "esphome/core/version.h"
#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif
#ifdef USE_ETHERNET
#include "esphome/components/ethernet/ethernet_component.h"
#endif
#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif
#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif
#ifdef USE_CONFIG_JSON
#include "esphome/components/config_json/config_json.h"
#endif
#ifdef USE_WEB_AUTH
#include "esphome/components/web_auth/web_auth.h"
#endif
#ifdef USE_ESP32
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#endif
#endif

namespace esphome::web_device_dashboard {

static const char *const TAG = "web_device_dashboard";
static const char *const API_PREFIX = "/api/device/";
static const size_t API_PREFIX_LEN = 12;
// A settings record is a few hundred bytes; nothing this API takes comes close.
static const size_t MAX_BODY_BYTES = 4096;
// The tail of the pretty MAC a confirmation has to carry: "DD:EE:FF", three octets.
static const size_t CONFIRM_TOKEN_LEN = 8;

// clang-format off
static const Route ROUTES[] = {
    {"info", RouteId::INFO, true, false},
    {"status", RouteId::STATUS, true, false},
    {"network", RouteId::NETWORK, true, false},
#ifdef USE_WEB_AUTH
    {"auth", RouteId::AUTH, true, true},
#endif
    {"capabilities", RouteId::CAPABILITIES, true, false},
    {"system/reboot", RouteId::SYSTEM_REBOOT, false, true},
    {"system/factory-reset", RouteId::SYSTEM_FACTORY_RESET, false, true},
    {"system/rollback", RouteId::SYSTEM_ROLLBACK, false, true},
#ifdef USE_CONFIG_JSON
    {"entities", RouteId::ENTITIES, true, false},
    {"entity-settings", RouteId::ENTITY_SETTINGS, true, true},
    {"entity-settings-meta", RouteId::ENTITY_SETTINGS_META, true, false},
#endif
};
// clang-format on

void WebDeviceDashboard::setup() {
  this->base_->init();
  this->base_->add_handler(this);
}

void WebDeviceDashboard::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Web Device Dashboard:\n"
                "  Page: / (v%s, %u bytes gzipped)\n"
                "  API: %s",
                INDEX_VERSION, static_cast<unsigned>(sizeof(INDEX_GZ)), API_PREFIX);
}

std::string WebDeviceDashboard::url_(AsyncWebServerRequest *request) const {
#ifdef USE_ESP32
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  return std::string(request->url_to(buf));
#else
  return request->url();
#endif
}

const Route *WebDeviceDashboard::route_for_(const std::string &url) const {
  if (!url.starts_with(API_PREFIX))
    return nullptr;
  const std::string tail = url.substr(API_PREFIX_LEN);
  for (const Route &route : ROUTES) {
    if (tail == route.name)
      return &route;
  }
  return nullptr;
}

// Everything under the prefix, so an unknown route is this API's 404 and not web_server's.
bool WebDeviceDashboard::canHandle(AsyncWebServerRequest *request) const {
  const std::string url = this->url_(request);
  return url == "/" || url.starts_with(API_PREFIX);
}

void WebDeviceDashboard::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                    size_t total) {
  if (index == 0) {
    this->body_.clear();
    this->body_total_ = total;
    this->body_received_ = 0;
    this->body_too_large_ = total > MAX_BODY_BYTES;
  }
  this->body_received_ += len;
  if (this->body_too_large_)
    return;
  this->body_.append(reinterpret_cast<const char *>(data), len);
}

void WebDeviceDashboard::handleRequest(AsyncWebServerRequest *request) {
  const std::string url = this->url_(request);
  // Verbose, not debug: the page polls status and network every few seconds.
  ESP_LOGV(TAG, "%s", url.c_str());
  // The buffer is this request's only if it is complete and sized for it.
  if (this->body_total_ != request->contentLength() || this->body_received_ != this->body_total_)
    this->reset_body_();
  const Route *route = this->route_for_(url);
  if (url == "/") {
    if (request->method() == HTTP_GET) {
      this->handle_page_(request);
    } else {
      this->send_status_(request, "405 Method Not Allowed", "GET", R"({"success":false,"error":"Method not allowed"})");
    }
  } else if (route == nullptr) {
    this->send_error_(request, 404, "Not found");
  } else if (this->check_method_(request, *route)) {
    switch (route->id) {
      case RouteId::INFO:
        this->handle_info_(request);
        break;
      case RouteId::STATUS:
        this->handle_status_(request);
        break;
      case RouteId::NETWORK:
        this->handle_network_(request);
        break;
#ifdef USE_WEB_AUTH
      case RouteId::AUTH:
        if (request->method() == HTTP_POST) {
          this->handle_auth_set_(request);
        } else {
          this->handle_auth_get_(request);
        }
        break;
#endif
      case RouteId::CAPABILITIES:
        this->handle_capabilities_(request);
        break;
      case RouteId::SYSTEM_REBOOT:
        this->handle_reboot_(request);
        break;
      case RouteId::SYSTEM_FACTORY_RESET:
        this->handle_factory_reset_(request);
        break;
      case RouteId::SYSTEM_ROLLBACK:
        this->handle_rollback_(request);
        break;
#ifdef USE_CONFIG_JSON
      case RouteId::ENTITIES:
        this->handle_entities_(request);
        break;
      case RouteId::ENTITY_SETTINGS:
        if (request->method() == HTTP_POST) {
          this->handle_entity_settings_set_(request);
        } else {
          this->handle_entity_settings_get_(request);
        }
        break;
      case RouteId::ENTITY_SETTINGS_META:
        this->handle_entity_settings_meta_(request);
        break;
#endif
    }
  }
  this->reset_body_();
}

// Releases the buffer rather than keep a request's worth of heap between calls.
void WebDeviceDashboard::reset_body_() {
  std::string().swap(this->body_);
  this->body_total_ = 0;
  this->body_received_ = 0;
  this->body_too_large_ = false;
}

bool WebDeviceDashboard::check_method_(AsyncWebServerRequest *request, const Route &route) {
  const auto method = request->method();
  if ((method == HTTP_GET && route.get) || (method == HTTP_POST && route.post))
    return true;
  const char *allow = !route.post ? "GET" : route.get ? "GET, POST" : "POST";
  ESP_LOGW(TAG, "Refusing method %d on %s, allowed: %s", static_cast<int>(method), route.name, allow);
  this->send_status_(request, "405 Method Not Allowed", allow, R"({"success":false,"error":"Method not allowed"})");
  return false;
}

// web_server_idf knows a handful of codes and turns every other one into a 500; the ones it
// does not know are written out by hand, at the price of the server's default headers.
static const char *status_line(int code) {
  switch (code) {
    case 403:
      return "403 Forbidden";
    case 413:
      return "413 Payload Too Large";
    case 415:
      return "415 Unsupported Media Type";
    case 503:
      return "503 Service Unavailable";
    default:
      return nullptr;
  }
}

void WebDeviceDashboard::send_error_(AsyncWebServerRequest *request, int code, const char *message) {
  auto body = json::build_json([message](JsonObject root) {
    root["success"] = false;
    root["error"] = message;
  });
  const char *status = status_line(code);
  if (status == nullptr) {
    request->send(code, "application/json", body.c_str());
    return;
  }
  this->send_status_(request, status, nullptr, body.c_str());
}

void WebDeviceDashboard::send_success_(AsyncWebServerRequest *request, const char *message) {
  auto body = json::build_json([message](JsonObject root) {
    root["success"] = true;
    root["message"] = message;
  });
  request->send(200, "application/json", body.c_str());
}

void WebDeviceDashboard::send_status_(AsyncWebServerRequest *request, const char *status, const char *allow,
                                      const char *body) {
#ifdef USE_ESP32
  // By hand because AsyncWebServerRequest::send() turns every status it does not
  // know into a 500, and because httpd_resp_set_hdr() keeps the pointer it is
  // given rather than a copy: every argument here is a literal.
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  if (allow != nullptr)
    httpd_resp_set_hdr(req, "Allow", allow);
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
#else
  request->send(atoi(status), "application/json", body);
#endif
}

void WebDeviceDashboard::handle_page_(AsyncWebServerRequest *request) {
  auto *response = request->beginResponse(200, "text/html", INDEX_GZ, sizeof(INDEX_GZ));
  response->addHeader("Content-Encoding", "gzip");
  response->addHeader("Cache-Control", "public, max-age=3600");
  request->send(response);
}

void WebDeviceDashboard::handle_info_(AsyncWebServerRequest *request) {
  auto body = json::build_json([this](JsonObject root) {
    // std::string values are copied into the document; a const char* into a local buffer is not.
    root["name"] = (App.get_friendly_name().empty() ? App.get_name() : App.get_friendly_name()).str();
    char mac[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
    std::string base_mac = get_mac_address_pretty_into_buffer(mac);
    root["base_mac_address"] = base_mac;
    std::string active_mac = base_mac;
#ifdef USE_ETHERNET
    if (ethernet::global_eth_component != nullptr)
      active_mac = ethernet::global_eth_component->get_eth_mac_address_pretty_into_buffer(mac);
#endif
    root["mac_address"] = active_mac;
    root["version"] = ESPHOME_VERSION;
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
    this->write_board_(root);
#endif
  });
  request->send(200, "application/json", body.c_str());
}

#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
// The identity jethome_board_info read from the CPU board's EEPROM, verbatim, plus the
// chip's own eFuse MACs in the header's two formats.
void WebDeviceDashboard::write_board_(JsonObject root) {
  using namespace jethome_board_info;
  auto *board = this->board_;
  JsonObject out = root["board"].to<JsonObject>();
  out["valid"] = board->is_valid();
  if (!board->is_valid())
    return;
  out["header_version"] = board->get_header_version();
  out["boardname"] = board->get_boardname();
  out["boardversion"] = board->get_boardversion();
  out["board_serial"] = board->get_board_serial();
  out["usid"] = board->get_usid();
  out["cpuid"] = board->get_cpuid();
  out["mac"] = format_mac_plain(board->get_mac());
  out["timestamp"] = board->get_timestamp();
  out["signature_version"] = board->get_signature_version();
  if (board->get_signature_version() != SIG_NONE)
    out["signature"] = format_hex(board->get_signature(), SIGNATURE_SIZE);
  if (board->has_device_identity()) {
    JsonObject identity = out["identity"].to<JsonObject>();
    identity["model"] = board->get_device_model();
    identity["serial"] = board->get_device_serial();
    identity["hw_revision"] = board->get_hw_revision();
    if (board->has_serial_check())
      identity["serial_matches_usid"] = board->serial_matches_usid();
  } else {
    out["identity"] = nullptr;
  }
#ifdef USE_ESP32
  JsonObject efuse = out["efuse"].to<JsonObject>();
  uint8_t mac[6];
  efuse["factory_mac"] = nullptr;
  if (esp_efuse_read_field_blob(ESP_EFUSE_MAC_FACTORY, mac, 48) == ESP_OK)
    efuse["factory_mac"] = format_mac(mac);
  efuse["custom_mac"] = nullptr;
#ifdef USE_ESP32_VARIANT_ESP32
  const esp_efuse_desc_t **custom = ESP_EFUSE_MAC_CUSTOM;
#else
  const esp_efuse_desc_t **custom = ESP_EFUSE_USER_DATA_MAC_CUSTOM;
#endif
  if (esp_efuse_read_field_blob(custom, mac, 48) == ESP_OK)
    efuse["custom_mac"] = format_mac_plain(mac);
#endif
  // What the identity card shows: the record's fields, or the serial a v3 header carries.
  if (!board->get_device_serial().empty())
    root["serial_number"] = board->get_device_serial();
  if (!board->get_device_model().empty())
    root["device_model"] = board->get_device_model();
  if (!board->get_hw_revision().empty())
    root["hw_revision"] = board->get_hw_revision();
}
#endif  // USE_WEB_DEVICE_DASHBOARD_BOARD_INFO

#ifdef USE_ESP32
static const char *reset_reason_name() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "power-on";
    case ESP_RST_EXT:
      return "external";
    case ESP_RST_SW:
      return "software";
    case ESP_RST_PANIC:
      return "panic";
    case ESP_RST_INT_WDT:
      return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "task watchdog";
    case ESP_RST_WDT:
      return "watchdog";
    case ESP_RST_DEEPSLEEP:
      return "deep sleep";
    case ESP_RST_BROWNOUT:
      return "brownout";
    case ESP_RST_SDIO:
      return "sdio";
    default:
      return "unknown";
  }
}
#endif

// Interface + address of the active link, shared by /status and /network.
struct LinkInfo {
  const char *connection{"none"};
  const char *netif_key{nullptr};
  bool rssi_valid{false};
  int8_t rssi{0};
};

static LinkInfo link_info() {
  LinkInfo info;
#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_connected()) {
    info.connection = "wifi";
    info.netif_key = "WIFI_STA_DEF";
    info.rssi_valid = true;
    info.rssi = wifi::global_wifi_component->wifi_rssi();
  }
#endif
#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr && ethernet::global_eth_component->is_connected()) {
    info.connection = "ethernet";
    info.netif_key = "ETH_DEF";
  }
#endif
  return info;
}

static void write_ip_address(JsonObject root) {
  root["ip_address"] = nullptr;
#ifdef USE_NETWORK
  for (const auto &ip : network::get_ip_addresses()) {
    if (ip.is_set()) {
      char buf[network::IP_ADDRESS_BUFFER_SIZE];
      root["ip_address"] = std::string(ip.str_to(buf));
      break;
    }
  }
#endif
}

void WebDeviceDashboard::handle_status_(AsyncWebServerRequest *request) {
  auto body = json::build_json([](JsonObject root) {
#ifdef USE_API
    root["ha_connected"] = api::global_api_server != nullptr && api::global_api_server->is_connected();
#endif
    // 64-bit: a 32-bit millis() would send the uptime back to zero every 49.7 days.
    root["uptime_s"] = millis_64() / 1000;
#ifdef USE_ESP32
    root["reset_reason"] = reset_reason_name();
#else
    root["reset_reason"] = "unknown";
#endif
    const LinkInfo link = link_info();
    root["connection_type"] = link.connection;
    root["rssi"] = nullptr;
    if (link.rssi_valid)
      root["rssi"] = link.rssi;
    write_ip_address(root);
    root["reboot_required"] = false;
  });
  request->send(200, "application/json", body.c_str());
}

#ifdef USE_ESP32
static void write_ip4(JsonObject root, const char *key, const esp_ip4_addr_t &addr) {
  char buf[16];
  if (addr.addr == 0) {
    root[key] = nullptr;
  } else {
    root[key] = std::string(esp_ip4addr_ntoa(&addr, buf, sizeof(buf)));
  }
}
#endif

// GET /api/device/network: the live link with gateway, subnet and DNS.
void WebDeviceDashboard::handle_network_(AsyncWebServerRequest *request) {
  auto body = json::build_json([](JsonObject root) {
    const LinkInfo link = link_info();
    root["hostname"] = App.get_name().str();
    root["connection_type"] = link.connection;
    write_ip_address(root);
    root["gateway"] = nullptr;
    root["subnet"] = nullptr;
    root["dns1"] = nullptr;
    root["dns2"] = nullptr;
#ifdef USE_ESP32
    esp_netif_t *netif = link.netif_key != nullptr ? esp_netif_get_handle_from_ifkey(link.netif_key) : nullptr;
    esp_netif_ip_info_t ip_info;
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
      write_ip4(root, "gateway", ip_info.gw);
      write_ip4(root, "subnet", ip_info.netmask);
    }
    esp_netif_dns_info_t dns;
    if (netif != nullptr && esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK)
      write_ip4(root, "dns1", dns.ip.u_addr.ip4);
    if (netif != nullptr && esp_netif_get_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK)
      write_ip4(root, "dns2", dns.ip.u_addr.ip4);
#endif
    root["ssid"] = nullptr;
    root["rssi"] = nullptr;
    if (link.rssi_valid) {
      root["rssi"] = link.rssi;
#ifdef USE_WIFI
      char ssid[wifi::SSID_BUFFER_SIZE];
      root["ssid"] = std::string(wifi::global_wifi_component->wifi_ssid_to(ssid));
#endif
    }
    bool eth_connected = false;
#ifdef USE_ETHERNET
    eth_connected = ethernet::global_eth_component != nullptr && ethernet::global_eth_component->is_connected();
#endif
    root["ethernet_connected"] = eth_connected;
  });
  request->send(200, "application/json", body.c_str());
}

#ifdef USE_WEB_AUTH
// GET /api/device/auth: who the server lets in, and whether that is still the factory pair.
void WebDeviceDashboard::handle_auth_get_(AsyncWebServerRequest *request) {
  auto *auth = web_auth::global_web_auth;
  if (auth == nullptr) {
    this->send_error_(request, 503, "Web auth not available");
    return;
  }
  const web_auth::WebAuth::Status status = auth->status();
  auto body = json::build_json([&status](JsonObject root) {
    root["username"] = status.username;
    root["password_length"] = status.password_length;
    root["is_default"] = status.is_default;
  });
  request->send(200, "application/json", body.c_str());
}

// The media type on its own: parameters dropped, surrounding space gone, case ignored. The
// one type, not anything that merely contains it.
static bool says_json(const optional<std::string> &content_type) {
  if (!content_type.has_value())
    return false;
  const std::string type = str_lower_case(str_until(content_type.value(), ';'));
  const size_t first = type.find_first_not_of(" \t");
  if (first == std::string::npos)
    return false;
  return type.compare(first, type.find_last_not_of(" \t") - first + 1, "application/json") == 0;
}

// POST {"username", "password"}. The new pair is checked here and applied from the loop task,
// so this request still answers under the old one and the browser is asked for the new one on
// the page's next call.
void WebDeviceDashboard::handle_auth_set_(AsyncWebServerRequest *request) {
  // A type no HTML form can send, so no page on another site can aim one here and have the
  // browser attach the credentials it has cached; the way back from this route is a trip to
  // the device's display menu.
  if (!says_json(request->get_header("Content-Type"))) {
    this->send_error_(request, 415, "Expected Content-Type: application/json");
    return;
  }
  if (this->body_too_large_) {
    this->send_error_(request, 413, "Request body over 4 KiB");
    return;
  }
  auto *auth = web_auth::global_web_auth;
  if (auth == nullptr) {
    this->send_error_(request, 503, "Web auth not available");
    return;
  }
  JsonDocument doc = json::parse_json(this->body_);
  if (doc.isNull() || !doc.is<JsonObject>()) {
    this->send_error_(request, 400, "Invalid JSON");
    return;
  }
  // A number or a null here would read as an empty string further down and shut the device
  // behind credentials nobody typed.
  if (!doc["username"].is<const char *>() || !doc["password"].is<const char *>()) {
    this->send_error_(request, 400, "'username' and 'password' must be strings");
    return;
  }
  // Straight to std::string: a JSON string may carry a NUL, and through a C string the field
  // would end there — a password stored shorter than the one that was sent, reported as saved.
  const std::string username = doc["username"].as<std::string>();
  const std::string password = doc["password"].as<std::string>();
  if (const char *error = web_auth::WebAuth::validate(username, password); error != nullptr) {
    this->send_error_(request, 400, error);
    return;
  }
  // Preferences are written from the loop task; storing them on the server's task would race
  // the pending-write list it flushes. Named, so a second POST arriving before the loop runs
  // replaces the first instead of queueing a second write of the pair the server is reading.
  this->defer("web-auth", [auth, username, password]() { auth->set_credentials(username, password); });
  this->send_success_(request, "Credentials updated");
}
#endif  // USE_WEB_AUTH

// GET /api/device/capabilities: what this firmware has, so the page knows which screens to
// draw and which routes exist. A key is present only when the capability is; one that has no
// detail to carry is `true`.
void WebDeviceDashboard::handle_capabilities_(AsyncWebServerRequest *request) {
  auto body = json::build_json([this](JsonObject root) {
    root["reboot"] = true;
    JsonObject factory_reset = root["factory_reset"].to<JsonObject>();
    factory_reset["clears_storage"] = false;
#ifdef USE_WEB_DEVICE_DASHBOARD_STORAGE
    if (this->storage_ != nullptr) {
      factory_reset["clears_storage"] = true;
      // What the mount is, not how full it is: usage is live, this route is read once, and
      // web_file_browser's own `info` already answers it from the same getter.
      JsonObject storage = root["storage"].to<JsonObject>();
      storage["type"] = this->storage_->get_filesystem_type();
      storage["base_path"] = this->storage_->get_base_path();
      storage["mounted"] = this->storage_->is_mounted();
    }
#endif
    const RollbackTarget target = this->rollback_target_();
    if (target.available()) {
      JsonObject rollback = root["rollback"].to<JsonObject>();
      rollback["partition"] = target.partition;
      if (!target.version.empty())
        rollback["version"] = target.version;
      if (!target.project_name.empty())
        rollback["project_name"] = target.project_name;
    }
    if (this->files_url_prefix_ != nullptr) {
      JsonObject files = root["files"].to<JsonObject>();
      files["url_prefix"] = this->files_url_prefix_;
    }
    if (this->automations_url_prefix_ != nullptr) {
      JsonObject automations = root["automations"].to<JsonObject>();
      automations["url_prefix"] = this->automations_url_prefix_;
    }
#ifdef USE_CONFIG_JSON
    auto *keeper = config_json::global_config_json_keeper;
    if (keeper != nullptr) {
      JsonObject entity_settings = root["entity_settings"].to<JsonObject>();
      JsonArray types = entity_settings["types"].to<JsonArray>();
      for (auto *settings : keeper->settings())
        types.add(settings->get_key());
    }
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
    root["board_info"] = true;
#endif
  });
  request->send(200, "application/json", body.c_str());
}

// A stray POST is one page load away, and all three system routes are one-way. The token is
// the tail of base_mac_address, so confirming means having read /api/device/info of this
// device rather than having followed a link. Not the active MAC: on a build with Ethernet
// that is a different one, and the answer names which is meant.
bool WebDeviceDashboard::check_confirm_(AsyncWebServerRequest *request) {
  if (this->body_too_large_) {
    this->send_error_(request, 413, "Request body over 4 KiB");
    return false;
  }
  JsonDocument doc = json::parse_json(this->body_);
  if (doc.isNull() || !doc.is<JsonObject>()) {
    this->send_error_(request, 400, "Invalid JSON");
    return false;
  }
  if (!(doc["confirm"] | false)) {
    this->send_error_(request, 400, "'confirm' must be true");
    return false;
  }
  char buf[MAC_ADDRESS_PRETTY_BUFFER_SIZE];
  const std::string mac = get_mac_address_pretty_into_buffer(buf);
  const std::string expected = mac.substr(mac.size() - CONFIRM_TOKEN_LEN);
  const char *token = doc["confirm_token"];
  if (token == nullptr || !str_equals_case_insensitive(token, expected)) {
    ESP_LOGW(TAG, "Refusing a system action: wrong confirm_token");
    this->send_error_(request, 403, "'confirm_token' must be the last three octets of base_mac_address");
    return false;
  }
  return true;
}

// POST /api/device/system/reboot
void WebDeviceDashboard::handle_reboot_(AsyncWebServerRequest *request) {
  if (!this->check_confirm_(request))
    return;
  ESP_LOGI(TAG, "Reboot requested over the API");
  this->send_success_(request, "Rebooting");
  this->reboot_();
}

// POST /api/device/system/factory-reset
void WebDeviceDashboard::handle_factory_reset_(AsyncWebServerRequest *request) {
  if (!this->check_confirm_(request))
    return;
  ESP_LOGW(TAG, "Factory reset requested over the API");
  this->send_success_(request, "Factory reset, rebooting");
  this->factory_reset_();
}

// POST /api/device/system/rollback: the other app slot becomes the next boot -- the firmware
// this one replaced, until a rollback makes the newer one the other slot. Availability is
// answered before the confirmation because it is about the firmware, not about the request.
// The image is checked here rather than optimistically, so a slot that turns out to be
// broken is an error the caller sees instead of a device that reboots and comes back the same.
void WebDeviceDashboard::handle_rollback_(AsyncWebServerRequest *request) {
  const RollbackTarget target = this->rollback_target_();
  if (!target.available()) {
    this->send_error_(request, 503, "No firmware to roll back to");
    return;
  }
  if (!this->check_confirm_(request))
    return;
  const char *error = this->select_rollback_(target);
  if (error != nullptr) {
    ESP_LOGE(TAG, "Rollback to '%s' failed: %s", target.partition.c_str(), error);
    this->send_error_(request, 500, error);
    return;
  }
  ESP_LOGW(TAG, "Rolling back to '%s'", target.partition.c_str());
  this->send_success_(request, "Rolling back, rebooting");
  this->reboot_();
}

#ifdef USE_ESP32
// The slot the next update would be written to is the one a rollback boots, and its app
// descriptor says which firmware that is. Only the descriptor is read here — a
// 256-byte header, not the image — because /capabilities is answered on every page load;
// whether the image behind it is whole is what esp_ota_set_boot_partition() then checks.
RollbackTarget WebDeviceDashboard::rollback_target_() const {
  RollbackTarget target;
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (other == nullptr)
    return target;
  esp_app_desc_t desc;
  if (esp_ota_get_partition_description(other, &desc) != ESP_OK)
    return target;
  target.partition = other->label;
  target.version = std::string(desc.version, strnlen(desc.version, sizeof(desc.version)));
  target.project_name = std::string(desc.project_name, strnlen(desc.project_name, sizeof(desc.project_name)));
  return target;
}

// Reads the whole image back and hashes it before it writes the boot selection, so this
// takes a moment on the server task. With rollback enabled — it is, through `ota:` — the
// slot is selected for one monitored boot: a firmware that dies before safe_mode marks it
// good brings the bootloader back to this one.
const char *WebDeviceDashboard::select_rollback_(const RollbackTarget &target) {
  const esp_partition_t *other = esp_ota_get_next_update_partition(nullptr);
  if (other == nullptr || target.partition != other->label)
    return "The firmware to roll back to is gone";
  const esp_err_t err = esp_ota_set_boot_partition(other);
  return err == ESP_OK ? nullptr : esp_err_to_name(err);
}
#else
RollbackTarget WebDeviceDashboard::rollback_target_() const { return {}; }

const char *WebDeviceDashboard::select_rollback_(const RollbackTarget & /*target*/) {
  return "Rollback needs an ESP32";
}
#endif

void WebDeviceDashboard::restart_() { App.safe_reboot(); }

// Both wait the answer out on the loop task: the server task is still holding the socket this
// was asked on.
void WebDeviceDashboard::reboot_() {
  this->set_timeout(this->action_delay_ms_, [this]() { this->restart_(); });
}

// The steps the display menu's Factory reset takes, in that order: the preferences erase
// takes all of NVS with it, so the record asking for the wipe has to be written after it.
// The partition itself is wiped at the next boot, before anything mounts it.
void WebDeviceDashboard::factory_reset_() {
  this->set_timeout(this->action_delay_ms_, [this]() {
    global_preferences->reset();
#ifdef USE_WEB_DEVICE_DASHBOARD_STORAGE
    if (this->storage_ != nullptr && !this->storage_->request_format())
      ESP_LOGE(TAG, "Wiping the user partition failed");
#endif
    this->restart_();
  });
}

#ifdef USE_CONFIG_JSON
template<typename T> static void write_entity_index(JsonObject root, const char *type, const T &entities) {
  JsonArray list = root[type].to<JsonArray>();
  for (auto *obj : entities) {
    if (obj->is_internal())
      continue;
    char buf[OBJECT_ID_MAX_LEN];
    JsonObject entry = list.add<JsonObject>();
    entry["source_name"] = obj->get_object_id_to(buf).str();
    entry["name"] = obj->get_name().str();
  }
}

// GET /api/device/entities: object_id (the settings key) and name of every entity
// that has a settings type. The web_server REST and SSE address entities by name.
void WebDeviceDashboard::handle_entities_(AsyncWebServerRequest *request) {
  auto *keeper = config_json::global_config_json_keeper;
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  if (keeper != nullptr) {
    for (auto *settings : keeper->settings()) {
      const char *key = settings->get_key();
#ifdef USE_SWITCH
      if (strcmp(key, "switch") == 0)
        write_entity_index(root, key, App.get_switches());
#endif
#ifdef USE_BINARY_SENSOR
      if (strcmp(key, "binary_sensor") == 0)
        write_entity_index(root, key, App.get_binary_sensors());
#endif
    }
  }
  std::string json;
  serializeJson(doc, json);
  request->send(200, "application/json", json.c_str());
}

// GET /api/device/entity-settings?type=switch[&source_name=relay_1]
void WebDeviceDashboard::handle_entity_settings_get_(AsyncWebServerRequest *request) {
  auto *keeper = config_json::global_config_json_keeper;
  if (keeper == nullptr) {
    this->send_error_(request, 503, "Config JSON keeper not available");
    return;
  }
  if (!request->hasParam("type")) {
    this->send_error_(request, 400, "Query parameter 'type' is required");
    return;
  }
  const std::string type = request->getParam("type")->value();
  auto *settings = keeper->get_settings(type.c_str());
  if (settings == nullptr) {
    this->send_error_(request, 404, "Settings type not found");
    return;
  }

  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  root["type"] = type;
  if (request->hasParam("source_name")) {
    const std::string source_name = request->getParam("source_name")->value();
    settings->write_json_single(root, config_json::SETTINGS_FILE_VERSION, source_name.c_str());
  } else {
    settings->write_json(root, config_json::SETTINGS_FILE_VERSION);
  }
  std::string json;
  serializeJson(doc, json);
  request->send(200, "application/json", json.c_str());
}

// POST {"type": ..., "source_name": ..., "settings": {...}} or {"type", "source_name", "action": "delete"}.
void WebDeviceDashboard::handle_entity_settings_set_(AsyncWebServerRequest *request) {
  if (this->body_too_large_) {
    this->send_error_(request, 413, "Request body over 4 KiB");
    return;
  }
  auto *keeper = config_json::global_config_json_keeper;
  if (keeper == nullptr) {
    this->send_error_(request, 503, "Config JSON keeper not available");
    return;
  }
  JsonDocument doc = json::parse_json(this->body_);
  if (doc.isNull() || !doc.is<JsonObject>()) {
    this->send_error_(request, 400, "Invalid JSON");
    return;
  }
  const char *type = doc["type"];
  if (type == nullptr || strlen(type) == 0) {
    this->send_error_(request, 400, "'type' is required");
    return;
  }
  // A scalar here reads as an empty object further down, which would silently save the
  // record with every field at its default. isUnbound(), not isNull(): an explicit null is a
  // value the caller wrote, not a key left out.
  if (!doc["settings"].isUnbound() && !doc["settings"].is<JsonObject>()) {
    this->send_error_(request, 400, "'settings' must be an object");
    return;
  }
  auto *settings = keeper->get_settings(type);
  if (settings == nullptr) {
    this->send_error_(request, 404, "Settings type not found");
    return;
  }

  const char *action = doc["action"];
  // Anything else here is a typo, not an update: the caller asked for something this API
  // does not have.
  if (!doc["action"].isUnbound() && (action == nullptr || strcmp(action, "delete") != 0)) {
    this->send_error_(request, 400, "'action' must be 'delete'");
    return;
  }
  if (action != nullptr) {
    void *record = settings->can_delete(doc.as<JsonObject>());
    if (record == nullptr) {
      this->send_error_(request, 400, "Cannot delete: record not found");
      return;
    }
    if (settings->delete_record(record))
      keeper->save(type);
    this->send_success_(request, "Entity was removed");
    return;
  }

  void *record = settings->update_record_from_json(doc.as<JsonObject>());
  if (record == nullptr) {
    this->send_error_(request, 400, "Failed to update settings record");
    return;
  }
  keeper->save(type);
  ESP_LOGI(TAG, "Entity settings updated for type '%s'", type);
  this->send_success_(request, "Settings updated");
  // Entities are driven from the loop task, not the server's. The whole type is applied
  // rather than this record: a delete arriving first would free the record under the defer.
  this->defer([settings]() { settings->apply(); });
}

// GET /api/device/entity-settings-meta: the form fields per settings type.
void WebDeviceDashboard::handle_entity_settings_meta_(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonObject root = doc.to<JsonObject>();
  JsonObject types = root["settings"].to<JsonObject>();
  auto *keeper = config_json::global_config_json_keeper;
  if (keeper != nullptr) {
    for (auto *settings : keeper->settings()) {
      JsonObject type_obj = types[settings->get_key()].to<JsonObject>();
      settings->write_settings_meta(type_obj);
    }
  }
  std::string json;
  serializeJson(doc, json);
  request->send(200, "application/json", json.c_str());
}
#endif  // USE_CONFIG_JSON

}  // namespace esphome::web_device_dashboard
