#include "web_device_dashboard.h"
#include <ArduinoJson.h>
#include "dashboard_index.h"
#include "esphome/components/json/json_util.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
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

// clang-format off
static const Route ROUTES[] = {
    {"info", RouteId::INFO, true, false},
    {"status", RouteId::STATUS, true, false},
    {"network", RouteId::NETWORK, true, false},
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
    case 413:
      return "413 Payload Too Large";
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
    root["uptime_s"] = millis() / 1000;
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
  auto *settings = keeper->get_settings(type);
  if (settings == nullptr) {
    this->send_error_(request, 404, "Settings type not found");
    return;
  }

  const char *action = doc["action"];
  if (action != nullptr && strcmp(action, "delete") == 0) {
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
