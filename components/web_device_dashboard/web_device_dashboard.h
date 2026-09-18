#pragma once

#include <cstdint>
#include <string>
#include <ArduinoJson.h>
#include "esphome/components/web_origin_guard/web_origin_guard.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
#include "esphome/components/jethome_board_info/jethome_board_info.h"
#endif
#ifdef USE_WEB_AUTH
#include "esphome/components/web_auth/web_auth.h"
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_STORAGE
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#endif

namespace esphome::web_device_dashboard {

enum class RouteId : uint8_t {
  INFO,
  STATUS,
  NETWORK,
#ifdef USE_WEB_AUTH
  AUTH,
#endif
  CAPABILITIES,
  SYSTEM_REBOOT,
  SYSTEM_FACTORY_RESET,
  SYSTEM_ROLLBACK,
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

/// The app slot a rollback would boot, empty when the other slot holds no app image.
struct RollbackTarget {
  std::string partition;
  std::string version;
  std::string project_name;
  bool available() const { return !this->partition.empty(); }
};

// The dashboard page at / and the device API under /api/device/: info (with the board's
// EEPROM identity when jethome_board_info is wired in), status, network, what the firmware
// can do, the three system actions, with web_auth the HTTP credentials, and with config_json
// the entity index, the entity settings and their form fields.
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
#ifdef USE_WEB_DEVICE_DASHBOARD_STORAGE
  void set_storage(filesystem_storage_abstract::FilesystemStorageAbstract *storage) { this->storage_ = storage; }
#endif
  // Where the other two web components serve, as this firmware configured them; nullptr when
  // it has none. Only /capabilities reads them.
  void set_files_url_prefix(const char *prefix) { this->files_url_prefix_ = prefix; }
  void set_automations_url_prefix(const char *prefix) { this->automations_url_prefix_ = prefix; }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  std::string url_(AsyncWebServerRequest *request) const;
  const Route *route_for_(const std::string &url) const;
  bool check_method_(AsyncWebServerRequest *request, const Route &route);
  /// Answers 415 itself when the request does not say its body is JSON.
  bool require_json_(AsyncWebServerRequest *request);
  void reset_body_();
  void handle_page_(AsyncWebServerRequest *request);
  void handle_info_(AsyncWebServerRequest *request);
  void handle_status_(AsyncWebServerRequest *request);
  void handle_network_(AsyncWebServerRequest *request);
#ifdef USE_WEB_AUTH
  void handle_auth_get_(AsyncWebServerRequest *request);
  void handle_auth_set_(AsyncWebServerRequest *request);
#endif
  void handle_capabilities_(AsyncWebServerRequest *request);
  void handle_reboot_(AsyncWebServerRequest *request);
  void handle_factory_reset_(AsyncWebServerRequest *request);
  void handle_rollback_(AsyncWebServerRequest *request);
  /// Answers 400, 403 or 413 itself when the body is not a confirmation of this device.
  bool check_confirm_(AsyncWebServerRequest *request);
  void reboot_();
  void factory_reset_();
  // Virtual so the host tests can watch these happen: the real ones end the process or move
  // the boot partition, and there is no second app slot to read off a host build.
  virtual RollbackTarget rollback_target_() const;
  /// nullptr once the next boot is the rolled-back slot, else why it is not.
  virtual const char *select_rollback_(const RollbackTarget &target);
  virtual void restart_();
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
  // What is registered on the server; this handler is only ever reached through it.
  web_origin_guard::WebOriginGuard guard_{this};
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
  jethome_board_info::JetHomeBoardInfo *board_{nullptr};
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_STORAGE
  filesystem_storage_abstract::FilesystemStorageAbstract *storage_{nullptr};
#endif
  const char *files_url_prefix_{nullptr};
  const char *automations_url_prefix_{nullptr};
  // How long the answer is given to leave the socket before the device stops serving it. A
  // member so the tests can drop it and run the deferred work in the same loop call.
  uint32_t action_delay_ms_{500};
  // The server handles one request at a time, so one body buffer is enough. body_total_ is
  // the Content-Length the buffer was sized for, so a request can tell the buffer is its own.
  std::string body_;
  size_t body_total_{0};
  size_t body_received_{0};
  bool body_too_large_{false};
};

}  // namespace esphome::web_device_dashboard
