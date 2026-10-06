#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <ArduinoJson.h>
#include "esphome/components/firmware_rollback/firmware_rollback.h"
#include "esphome/components/loop_job/loop_job.h"
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
#ifdef USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS
#include "esphome/components/dallas_scan/dallas_scan.h"
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_MODBUS_MAP
#include "esphome/components/modbus_map/modbus_map.h"
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
#ifdef USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS
  TEMPERATURE_SLOTS,
  TEMPERATURE_SLOTS_FORGET,
  TEMPERATURE_SLOTS_ASSIGN,
  TEMPERATURE_SLOTS_OFFSET,
#endif
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

using firmware_rollback::RollbackTarget;

// The dashboard page at / and the device API under /api/device/: info (with the board's
// EEPROM identity when jethome_board_info is wired in), status, network, what the firmware
// can do, the three system actions, with web_auth the HTTP credentials, with dallas_scan the
// temperature slots, forgetting and assigning them and their offsets, and with config_json the
// entity index, the entity settings and their form fields. A modbus_map is reported in
// /capabilities.
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
#ifdef USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS
  void set_temperature_slots(dallas_scan::DallasScan *slots) { this->temperature_slots_ = slots; }
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_MODBUS_MAP
  void set_modbus_map(const modbus_map::ModbusMap *map) { this->modbus_map_ = map; }
#endif
  // Where the other web components serve, as this firmware configured them; nullptr when it
  // has none. Only /capabilities reads them.
  void set_files_url_prefix(const char *prefix) { this->files_url_prefix_ = prefix; }
  void set_automations_url_prefix(const char *prefix) { this->automations_url_prefix_ = prefix; }
  void set_climates_url_prefix(const char *prefix) { this->climates_url_prefix_ = prefix; }

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
  /// Answers 400, 413 or 415 itself when the body is not a JSON object; else it is left in @p doc.
  bool read_json_body_(AsyncWebServerRequest *request, JsonDocument &doc);
  /// Answers 400, 403, 413 or 415 itself when the body is not a confirmation of this device. The
  /// parsed body is left in @p doc for a route that takes more than the confirmation.
  bool check_confirm_(AsyncWebServerRequest *request, JsonDocument &doc);
  bool check_confirm_(AsyncWebServerRequest *request) {
    JsonDocument doc;
    return this->check_confirm_(request, doc);
  }
  void reboot_();
  void factory_reset_();
  // Virtual so the host tests can watch these happen: the real ones end the process or move
  // the boot partition, and there is no second app slot to read off a host build.
  virtual RollbackTarget rollback_target_() const;
  /// nullptr once the next boot is the rolled-back slot, else why it is not. Loop task only.
  virtual const char *select_rollback_(const RollbackTarget &target);
  virtual void restart_();
  /// Hands @p job to the loop task, which owns the entity records and the rollback, and waits
  /// for it. False when the loop never got to it: the job did not run and never will.
  /// Virtual for the same reason: a host build has one task, so nothing crosses on its own
  /// and only a stand-in can refuse a job or count what was handed over.
  virtual bool run_on_loop_(std::function<bool()> &&job);
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
  void write_board_(JsonObject root);
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_MODBUS_MAP
  static void write_modbus_map_(JsonObject modbus, const modbus_map::ModbusMap &map);
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS
  void handle_temperature_slots_(AsyncWebServerRequest *request);
  void handle_temperature_slots_forget_(AsyncWebServerRequest *request);
  void handle_temperature_slots_assign_(AsyncWebServerRequest *request);
  void handle_temperature_slots_offset_(AsyncWebServerRequest *request);
  static std::string temperature_slots_json_(dallas_scan::DallasScan *scan);
  void send_slot_change_(AsyncWebServerRequest *request, std::string message, bool reboot_required,
                         const char *waits = "; applies after a reboot");
  bool check_slots_writable_(AsyncWebServerRequest *request, dallas_scan::DallasScan *scan);
  /// The body's `slot`, 1 to max_sensors(), as an index; answers 400 itself when it is not one.
  bool read_slot_(AsyncWebServerRequest *request, JsonVariant value, size_t &slot);
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
  // A request arrives on the HTTP server's own task; a write goes over to the loop task.
  loop_job::LoopDispatcher dispatcher_;
#ifdef USE_WEB_DEVICE_DASHBOARD_BOARD_INFO
  jethome_board_info::JetHomeBoardInfo *board_{nullptr};
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_STORAGE
  filesystem_storage_abstract::FilesystemStorageAbstract *storage_{nullptr};
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_TEMPERATURE_SLOTS
  dallas_scan::DallasScan *temperature_slots_{nullptr};
#endif
#ifdef USE_WEB_DEVICE_DASHBOARD_MODBUS_MAP
  const modbus_map::ModbusMap *modbus_map_{nullptr};
#endif
  const char *files_url_prefix_{nullptr};
  const char *automations_url_prefix_{nullptr};
  const char *climates_url_prefix_{nullptr};
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
