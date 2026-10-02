#include "web_device_dashboard.h"
#include <ArduinoJson.h>
#include <cinttypes>
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
#ifdef USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS
    {"temperature-slots", RouteId::TEMPERATURE_SLOTS, true, false},
    {"temperature-slots/forget", RouteId::TEMPERATURE_SLOTS_FORGET, false, true},
    {"temperature-slots/assign", RouteId::TEMPERATURE_SLOTS_ASSIGN, false, true},
#endif
#ifdef USE_CONFIG_JSON
    {"entities", RouteId::ENTITIES, true, false},
    {"entity-settings", RouteId::ENTITY_SETTINGS, true, true},
    {"entity-settings-meta", RouteId::ENTITY_SETTINGS_META, true, false},
#endif
};
// clang-format on

void WebDeviceDashboard::setup() {
  this->dispatcher_.capture_loop_task();
  this->base_->init();
  this->base_->add_handler(&this->guard_);
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
#ifdef USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS
      case RouteId::TEMPERATURE_SLOTS:
        this->handle_temperature_slots_(request);
        break;
      case RouteId::TEMPERATURE_SLOTS_FORGET:
        this->handle_temperature_slots_forget_(request);
        break;
      case RouteId::TEMPERATURE_SLOTS_ASSIGN:
        this->handle_temperature_slots_assign_(request);
        break;
#endif
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
    case 409:
      return "409 Conflict";
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

// Every route that reads a JSON body demands the type that names one. No HTML form can send it,
// so a page on another site cannot steer a browser's cached credentials at these routes even
// where the request carries no Origin for web_origin_guard to judge.
bool WebDeviceDashboard::require_json_(AsyncWebServerRequest *request) {
  if (says_json(request->get_header("Content-Type")))
    return true;
  this->send_error_(request, 415, "Expected Content-Type: application/json");
  return false;
}

// POST {"username", "password"}. The new pair is checked here and applied from the loop task,
// so this request still answers under the old one and the browser is asked for the new one on
// the page's next call.
void WebDeviceDashboard::handle_auth_set_(AsyncWebServerRequest *request) {
  if (!this->require_json_(request))
    return;
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
      // What the mount is, not how full it is: usage is live, this route is not polled, and
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
#ifdef USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS
    if (this->temperature_slots_ != nullptr)
      root["temperature_slots"] = true;
#endif
  });
  request->send(200, "application/json", body.c_str());
}

// A stray POST is one page load away, and every route that takes this is one-way. The token is
// the tail of base_mac_address, so confirming means having read /api/device/info of this
// device rather than having followed a link. Not the active MAC: on a build with Ethernet
// that is a different one, and the answer names which is meant.
bool WebDeviceDashboard::check_confirm_(AsyncWebServerRequest *request, JsonDocument &doc) {
  if (!this->require_json_(request))
    return false;
  if (this->body_too_large_) {
    this->send_error_(request, 413, "Request body over 4 KiB");
    return false;
  }
  doc = json::parse_json(this->body_);
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

// POST /api/device/system/rollback: the other app slot becomes the next boot, when
// firmware_rollback says it holds one the bootloader would boot. Availability is answered
// before the confirmation because it is about the firmware, not about the request. The select
// runs on the loop task: the display menu selects there too, and the check and the switch
// must not interleave with it; it also keeps the image hash off the server task's stack (#64).
void WebDeviceDashboard::handle_rollback_(AsyncWebServerRequest *request) {
  const RollbackTarget target = this->rollback_target_();
  if (!target.available()) {
    this->send_error_(request, 503, "No firmware to roll back to");
    return;
  }
  if (!this->check_confirm_(request))
    return;
  const char *error = nullptr;
  const bool selected = this->run_on_loop_([&]() {
    error = this->select_rollback_(target);
    return error == nullptr;
  });
  if (!selected) {
    // No error means the loop task never took the job: nothing was selected and nothing will be.
    this->send_error_(request, error == nullptr ? 503 : 500, error == nullptr ? "Device busy" : error);
    return;
  }
  this->send_success_(request, "Rolling back, rebooting");
  this->reboot_();
}

RollbackTarget WebDeviceDashboard::rollback_target_() const { return firmware_rollback::rollback_target(); }

const char *WebDeviceDashboard::select_rollback_(const RollbackTarget &target) {
  return firmware_rollback::select_rollback(target);
}

void WebDeviceDashboard::restart_() { App.safe_reboot(); }

bool WebDeviceDashboard::run_on_loop_(std::function<bool()> &&job) {
  return this->dispatcher_.run_on_loop(this, std::move(job));
}

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
      ESP_LOGE(TAG, "The user partition was not wiped and will not be");
#endif
    this->restart_();
  });
}

#ifdef USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS
// GET /api/device/temperature-slots: the dallas_scan slots up to the last bound one, numbered
// from 1 as the sensor names and the log number them. A freed slot between bound ones keeps its
// row, as it does in the panel's Temperatures menu. Read straight off the component: the table
// only changes at setup and in forget() and assign(), which reboot.
void WebDeviceDashboard::handle_temperature_slots_(AsyncWebServerRequest *request) {
  auto *scan = this->temperature_slots_;
  if (scan == nullptr) {
    this->send_error_(request, 404, "No temperature slots");
    return;
  }
  auto body = json::build_json([scan](JsonObject root) {
    root["max_slots"] = scan->max_sensors();
    JsonArray slots = root["slots"].to<JsonArray>();
    for (size_t slot = 0; slot < scan->used_slots(); slot++) {
      JsonObject entry = slots.add<JsonObject>();
      entry["slot"] = slot + 1;
      entry["name"] = scan->slot_name(slot);
      entry["free"] = scan->sensor(slot) == nullptr;
      entry["listed"] = scan->pinned(slot);
      // A string: a 64-bit ROM does not survive a JavaScript number.
      if (const uint64_t rom = scan->address(slot); rom != 0)
        entry["address"] = str_sprintf("0x%016" PRIx64, rom);
      entry["can_forget"] = scan->can_forget(slot);
    }
  });
  request->send(200, "application/json", body.c_str());
}

// POST /api/device/temperature-slots/forget: {"slot": N} or {"all": true}, confirmed as the
// system actions are. What would change nothing is refused before anything happens, rather than
// answered with a reboot that leaves the table as it was; the rest answers, then forget() empties
// the slots and reboots, as the panel's Confirm does.
void WebDeviceDashboard::handle_temperature_slots_forget_(AsyncWebServerRequest *request) {
  auto *scan = this->temperature_slots_;
  if (scan == nullptr) {
    this->send_error_(request, 404, "No temperature slots");
    return;
  }
  JsonDocument doc;
  if (!this->check_confirm_(request, doc))
    return;
  if (!this->check_slots_writable_(request, scan))
    return;
  // By presence, not value: {"slot": 1, "all": false} names both, and which one won would be a
  // guess. isUnbound(), not isNull(): an explicit null is a key the caller wrote.
  const bool has_slot = !doc["slot"].isUnbound();
  const bool all = !doc["all"].isUnbound();
  if (has_slot && all) {
    this->send_error_(request, 400, "'slot' and 'all' exclude each other");
    return;
  }
  if (!has_slot && !all) {
    this->send_error_(request, 400, "'slot' or 'all' is required");
    return;
  }
  JsonVariant number = doc["slot"];
  JsonVariant every = doc["all"];
  if (all && !(every.is<bool>() && every.as<bool>())) {
    this->send_error_(request, 400, "'all' must be true");
    return;
  }
  int slot = -1;
  if (!all) {
    size_t index;
    if (!this->read_slot_(request, number, index))
      return;
    slot = static_cast<int>(index);
  }
  if (!scan->can_forget(slot)) {
    std::string why = all                  ? std::string("Nothing to forget: every slot is free or listed in YAML")
                      : scan->pinned(slot) ? str_sprintf("Slot %d belongs to a sensor listed in YAML", slot + 1)
                                           : str_sprintf("Slot %d is free", slot + 1);
    this->send_error_(request, 409, why.c_str());
    return;
  }
  if (all) {
    ESP_LOGW(TAG, "Forgetting every temperature slot over the API");
    this->send_success_(request, "Forgetting every slot, rebooting");
  } else {
    ESP_LOGW(TAG, "Forgetting temperature slot %d over the API", slot + 1);
    this->send_success_(request, str_sprintf("Forgetting slot %d, rebooting", slot + 1).c_str());
  }
  this->set_timeout(this->action_delay_ms_, [scan, slot]() { scan->forget(slot); });
}

// "0x" (or nothing) and 16 hex digits, the way /temperature-slots and the panel print a ROM.
static bool parse_rom(const char *text, uint64_t &rom) {
  if (text == nullptr)
    return false;
  if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    text += 2;
  if (strlen(text) != 16)
    return false;
  rom = 0;
  for (const char *c = text; *c != '\0'; c++) {
    const uint8_t digit = parse_hex_char(*c);
    if (digit == INVALID_HEX_CHAR)
      return false;
    rom = (rom << 4) | static_cast<uint64_t>(digit);
  }
  return true;
}

// POST /api/device/temperature-slots/assign: {"slot": N, "address": "0x…"}, confirmed as the
// system actions are. The device goes into slot N: a device already in another slot swaps with
// what slot N held, a new one takes slot N from its device. What dallas_scan would refuse is
// refused here first, with the reason; the rest answers, then assign() saves and reboots.
void WebDeviceDashboard::handle_temperature_slots_assign_(AsyncWebServerRequest *request) {
  auto *scan = this->temperature_slots_;
  if (scan == nullptr) {
    this->send_error_(request, 404, "No temperature slots");
    return;
  }
  JsonDocument doc;
  if (!this->check_confirm_(request, doc))
    return;
  if (!this->check_slots_writable_(request, scan))
    return;
  size_t slot;
  if (!this->read_slot_(request, doc["slot"], slot))
    return;
  uint64_t rom;
  JsonVariant address = doc["address"];
  if (!address.is<const char *>() || !parse_rom(address.as<const char *>(), rom)) {
    this->send_error_(request, 400, "'address' must be 16 hex digits, after an optional 0x");
    return;
  }
  const std::string hex = str_sprintf("0x%016" PRIx64, rom);
  switch (scan->check_assign(slot, rom)) {
    case dallas_scan::AssignCheck::OK:
      break;
    case dallas_scan::AssignCheck::BAD_SLOT:  // read_slot_ checked the range already
    case dallas_scan::AssignCheck::BAD_ADDRESS:
      this->send_error_(request, 400, (hex + " is not a thermometer ROM: wrong family or CRC").c_str());
      return;
    case dallas_scan::AssignCheck::LISTED_SLOT:
      this->send_error_(request, 409,
                        str_sprintf("Slot %u belongs to a sensor listed in YAML", (unsigned) slot + 1).c_str());
      return;
    case dallas_scan::AssignCheck::LISTED_ADDRESS:
      this->send_error_(request, 409, (hex + " belongs to a sensor listed in YAML").c_str());
      return;
    case dallas_scan::AssignCheck::UNCHANGED:
      this->send_error_(request, 409,
                        str_sprintf("%s is in slot %u already", hex.c_str(), (unsigned) slot + 1).c_str());
      return;
  }
  ESP_LOGW(TAG, "Assigning %s to temperature slot %u over the API", hex.c_str(), (unsigned) slot + 1);
  this->send_success_(request, str_sprintf("Assigning slot %u, rebooting", (unsigned) slot + 1).c_str());
  this->set_timeout(this->action_delay_ms_, [scan, slot, rom]() { scan->assign(slot, rom); });
}

// A file table whose partition did not mount: forget() and assign() would change nothing, so the
// request is refused rather than answered with a reboot that leaves the table as it was.
bool WebDeviceDashboard::check_slots_writable_(AsyncWebServerRequest *request, dallas_scan::DallasScan *scan) {
  if (scan->can_save())
    return true;
  this->send_error_(request, 503, "Temperature slot storage unavailable");
  return false;
}

bool WebDeviceDashboard::read_slot_(AsyncWebServerRequest *request, JsonVariant value, size_t &slot) {
  const int max = static_cast<int>(this->temperature_slots_->max_sensors());
  if (!value.is<int>() || value.as<int>() < 1 || value.as<int>() > max) {
    this->send_error_(request, 400, str_sprintf("'slot' must be a number from 1 to %d", max).c_str());
    return false;
  }
  slot = static_cast<size_t>(value.as<int>() - 1);
  return true;
}
#endif  // USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS

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
  const bool one = request->hasParam("source_name");
  const std::string source_name = one ? request->getParam("source_name")->value() : std::string();

  // The record list belongs to the loop task — a settings row on the display appends to it —
  // and this runs on the HTTP server's. The whole response is built there: a record read here
  // could be one the menu has just reallocated away.
  std::string json;
  int code = 0;
  const char *message = nullptr;
  const bool read = this->run_on_loop_([&]() {
    auto *settings = keeper->get_settings(type.c_str());
    if (settings == nullptr) {
      code = 404;
      message = "Settings type not found";
      return false;
    }
    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    root["type"] = type;
    if (one) {
      settings->write_json_single(root, config_json::SETTINGS_FILE_VERSION, source_name.c_str());
    } else {
      settings->write_json(root, config_json::SETTINGS_FILE_VERSION);
    }
    serializeJson(doc, json);
    return true;
  });

  if (!read) {
    // No code means the loop task never took the job: there is nothing to answer with.
    this->send_error_(request, code == 0 ? 503 : code, code == 0 ? "Device busy" : message);
    return;
  }
  request->send(200, "application/json", json.c_str());
}

// POST {"type": ..., "source_name": ..., "settings": {...}} or {"type", "source_name", "action": "delete"}.
void WebDeviceDashboard::handle_entity_settings_set_(AsyncWebServerRequest *request) {
  if (!this->require_json_(request))
    return;
  if (this->body_too_large_) {
    this->send_error_(request, 413, "Request body over 4 KiB");
    return;
  }
  auto *keeper = config_json::global_config_json_keeper;
  if (keeper == nullptr) {
    this->send_error_(request, 503, "Config JSON keeper not available");
    return;
  }
  // Asked here as well as on the loop task below, where the answer is the one that counts:
  // a device that cannot write has nothing to say about the rest of the request.
  if (!keeper->can_save()) {
    this->send_error_(request, 503, "Settings storage unavailable");
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
  const char *action = doc["action"];
  // Anything else here is a typo, not an update: the caller asked for something this API
  // does not have.
  if (!doc["action"].isUnbound() && (action == nullptr || strcmp(action, "delete") != 0)) {
    this->send_error_(request, 400, "'action' must be 'delete'");
    return;
  }

  // The record list, the keeper's save timer and the entities all belong to the loop task —
  // the display menu walks the same vector on every redraw — and this runs on the HTTP
  // server's. The check and the write go over together: split, the answer would describe a
  // device that had already moved on. `doc` outlives the call because run_on_loop blocks.
  int code = 0;
  const char *message = nullptr;
  const bool wrote = this->run_on_loop_([&]() {
    auto *settings = keeper->get_settings(type);
    if (settings == nullptr) {
      code = 404;
      message = "Settings type not found";
      return false;
    }
    // Asked before anything is applied: an accepted change that only lives in RAM would be
    // gone at the next reboot, and the device would have answered that it had kept it.
    if (!keeper->can_save()) {
      code = 503;
      message = "Settings storage unavailable";
      return false;
    }
    if (action != nullptr) {
      void *record = settings->can_delete(doc.as<JsonObject>());
      if (record == nullptr) {
        code = 400;
        message = "Cannot delete: record not found";
        return false;
      }
      if (settings->delete_record(record))
        keeper->save(type);
      message = "Entity was removed";
      return true;
    }
    if (settings->update_record_from_json(doc.as<JsonObject>()) == nullptr) {
      code = 400;
      message = "Failed to update settings record";
      return false;
    }
    keeper->save(type);
    ESP_LOGI(TAG, "Entity settings updated for type '%s'", type);
    // The whole type, not the record just written: applying one is the settings type's own
    // business and nothing here knows which entities a record reaches.
    settings->apply();
    message = "Settings updated";
    return true;
  });

  if (!wrote) {
    // No code means the loop task never took the job: nothing was changed and nothing will be.
    this->send_error_(request, code == 0 ? 503 : code, code == 0 ? "Device busy" : message);
    return;
  }
  this->send_success_(request, message);
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
