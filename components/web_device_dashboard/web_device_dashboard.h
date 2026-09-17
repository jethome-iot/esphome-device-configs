#pragma once

#include <cstdint>
#include <string>
#include <ArduinoJson.h>
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
#include "esphome/components/jethome_board_info/jethome_board_info.h"
#endif

namespace esphome::web_device_dashboard {

enum class RouteId : uint8_t {
  INFO,
  STATUS,
  NETWORK,
#ifdef USE_CONFIG_JSON
  ENTITIES,
  ENTITY_SETTINGS,
  ENTITY_SETTINGS_META,
#endif
};

struct Route {
  const char *name;
  RouteId id;
  /// Reads answer GET, writes POST; entity-settings reads on GET and writes on POST.
  bool get;
  bool post;
};

// The dashboard page at / and the device API under /api/device/: info (with the board's
// EEPROM identity when jethome_board_info is wired in), status, network, and with
// config_json the entity index, the entity settings and their form fields.
class WebDeviceDashboard : public AsyncWebHandler, public Component {
 public:
  explicit WebDeviceDashboard(web_server_base::WebServerBase *base) : base_(base) {}

  void setup() override;
  void dump_config() override;
  // Registered before web_server's handler so / wins over its index page.
  float get_setup_priority() const override { return setup_priority::WIFI - 0.5f; }

#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
  void set_board_info(jethome_board_info::JetHomeBoardInfo *board) { this->board_ = board; }
#endif

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  std::string url_(AsyncWebServerRequest *request) const;
  const Route *route_for_(const std::string &url) const;
  bool check_method_(AsyncWebServerRequest *request, const Route &route);
  void reset_body_();
  void handle_page_(AsyncWebServerRequest *request);
  void handle_info_(AsyncWebServerRequest *request);
  void handle_status_(AsyncWebServerRequest *request);
  void handle_network_(AsyncWebServerRequest *request);
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
  void write_board_(JsonObject root);
#endif
#ifdef USE_CONFIG_JSON
  void handle_entities_(AsyncWebServerRequest *request);
  void handle_entity_settings_get_(AsyncWebServerRequest *request);
  void handle_entity_settings_set_(AsyncWebServerRequest *request);
  void handle_entity_settings_meta_(AsyncWebServerRequest *request);
#endif
  void send_error_(AsyncWebServerRequest *request, int code, const char *message);
  void send_success_(AsyncWebServerRequest *request, const char *message);
  void send_status_(AsyncWebServerRequest *request, const char *status, const char *allow, const char *body);

  web_server_base::WebServerBase *base_;
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
  jethome_board_info::JetHomeBoardInfo *board_{nullptr};
#endif
  // The server handles one request at a time, so one body buffer is enough. body_total_ is
  // the Content-Length the buffer was sized for, so a request can tell the buffer is its own.
  std::string body_;
  size_t body_total_{0};
  size_t body_received_{0};
  bool body_too_large_{false};
};

}  // namespace esphome::web_device_dashboard
