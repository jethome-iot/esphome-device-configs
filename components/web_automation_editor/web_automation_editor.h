#pragma once

#include <cstdint>
#include <string>
#include "esphome/components/automations/automation_storage.h"
#include "esphome/components/web_origin_guard/web_origin_guard.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

namespace esphome::web_automation_editor {

enum class RouteId : uint8_t { LIST, GET, SAVE, DELETE, EXPORT, ENTITIES, SCHEMA, REBOOT, PING };

struct Route {
  const char *name;
  RouteId id;
  /// Mutating routes answer POST only, the rest GET only: a GET that writes is one
  /// <img src> away from being fired by any page the browser opens.
  bool mutating;
};

// JSON API for the automations component at <url_prefix>/api/*. The dashboard's
// editor is its client; the TS contract lives in client/.
class WebAutomationEditor : public AsyncWebHandler, public Component {
 public:
  WebAutomationEditor(web_server_base::WebServerBase *base, automations::AutomationStorage *storage)
      : base_(base), storage_(storage) {}

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override {
    // After WiFi. Nothing orders this against web_server: WIFI - 1.0f is exactly
    // web_server's own priority, so the two fall to registration order.
    return setup_priority::WIFI - 1.0f;
  }

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  // For the Arduino server, which hands a raw body only to a handler that says so;
  // web_server_idf gives it to the first handler that claims the request regardless.
  bool isRequestHandlerTrivial() const override { return false; }

  void set_url_prefix(const std::string &prefix) { this->url_prefix_ = prefix; }
  const std::string &get_url_prefix() const { return this->url_prefix_; }

 protected:
  std::string url_(AsyncWebServerRequest *request) const;
  const Route *route_for_(const std::string &url) const;
  const automations::AutomationConfig *find_(uint32_t id) const;
  // Answers 400 itself when the id parameter is missing or not a number, so a false return
  // is a finished request.
  bool read_id_(AsyncWebServerRequest *request, uint32_t &id);
  void reset_body_();
  // Answers 405 itself when the method is wrong, so a false return is a finished request.
  bool check_method_(AsyncWebServerRequest *request, const Route &route);
  void handle_list_(AsyncWebServerRequest *request);
  void handle_get_(AsyncWebServerRequest *request);
  void handle_save_(AsyncWebServerRequest *request);
  void handle_delete_(AsyncWebServerRequest *request);
  void handle_export_(AsyncWebServerRequest *request);
  void handle_entities_(AsyncWebServerRequest *request);
  void handle_schema_(AsyncWebServerRequest *request);
  void send_error_(AsyncWebServerRequest *request, const std::string &message, int code = 400);
  void send_success_(AsyncWebServerRequest *request, const char *message, uint32_t id = 0);
  // For the statuses AsyncWebServerRequest::send() does not know and would turn into a 500.
  void send_status_(AsyncWebServerRequest *request, const char *status, const char *allow, const char *body);
  // A seam: the tests cannot survive App.safe_reboot().
  virtual void reboot_();

  web_server_base::WebServerBase *base_;
  automations::AutomationStorage *storage_;
  // What is registered on the server; this handler is only ever reached through it.
  web_origin_guard::WebOriginGuard guard_{this};
  std::string url_prefix_{"/automation-editor"};
  // The server handles one request at a time, so one body buffer is enough. body_total_ is
  // the Content-Length the buffer belongs to: a receive that failed midway never reaches
  // handleRequest, so the next request checks it against its own before trusting the buffer.
  std::string body_;
  size_t body_total_{0};
  size_t body_received_{0};
  bool body_too_large_{false};
};

}  // namespace esphome::web_automation_editor
