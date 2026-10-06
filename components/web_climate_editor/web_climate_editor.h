#pragma once

#include <cstdint>
#include <string>
#include "esphome/components/climate_hub/climate_hub.h"
#include "esphome/components/web_origin_guard/web_origin_guard.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"

namespace esphome::web_climate_editor {

enum class RouteId : uint8_t {
  LIST,
  GET,
  STATUS,
  ENTITIES,
  SCHEMA,
  PING,
  SAVE,
  IMPORT,
  DELETE,
  ENABLE,
  SETPOINT,
  PRESET
};

struct Route {
  const char *name;
  RouteId id;
  /// Mutating routes answer POST only, the rest GET only: a GET that writes is one
  /// <img src> away from being fired by any page the browser opens.
  bool mutating;
};

// JSON API for the climate_hub thermostats at <url_prefix>/api/*. The dashboard's editor is
// its client; the TS contract lives in client/.
class WebClimateEditor : public AsyncWebHandler, public Component {
 public:
  WebClimateEditor(web_server_base::WebServerBase *base, climate_hub::ClimateHub *hub) : base_(base), hub_(hub) {}

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override {
    // After WiFi, at web_server's own priority, like the automation editor: the url_prefix
    // validator keeps the two apart instead.
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
  void reset_body_();
  // Each of these answers 400 or 405 itself, so a false return is a finished request.
  bool check_method_(AsyncWebServerRequest *request, const Route &route);
  bool read_id_(AsyncWebServerRequest *request, std::string &id) { return this->read_slug_(request, "id", id); }
  bool read_slug_(AsyncWebServerRequest *request, const char *name, std::string &value);
  bool read_bool_(AsyncWebServerRequest *request, const char *name, bool &value);
  bool read_number_(AsyncWebServerRequest *request, float &value);

  void handle_list_(AsyncWebServerRequest *request);
  void handle_get_(AsyncWebServerRequest *request);
  void handle_status_(AsyncWebServerRequest *request);
  void handle_entities_(AsyncWebServerRequest *request);
  void handle_schema_(AsyncWebServerRequest *request);
  void handle_document_(AsyncWebServerRequest *request, bool importing);
  void handle_delete_(AsyncWebServerRequest *request);
  void handle_enable_(AsyncWebServerRequest *request);
  void handle_setpoint_(AsyncWebServerRequest *request);
  void handle_preset_(AsyncWebServerRequest *request);

  /// Answers what a job decided: `json` when it ran and succeeded, the error it set when it
  /// ran and refused, 503 when the loop task never took it.
  void answer_(AsyncWebServerRequest *request, bool ran, int code, const std::string &error, const std::string &json);
  void send_json_(AsyncWebServerRequest *request, int code, const std::string &json);
  void send_error_(AsyncWebServerRequest *request, const std::string &message, int code = 400);
  // For the statuses AsyncWebServerRequest::send() does not know and would turn into a 500.
  void send_status_(AsyncWebServerRequest *request, const char *status, const char *allow, const char *body);

  web_server_base::WebServerBase *base_;
  climate_hub::ClimateHub *hub_;
  // What is registered on the server; this handler is only ever reached through it.
  web_origin_guard::WebOriginGuard guard_{this};
  std::string url_prefix_{"/climate-editor"};
  // The server handles one request at a time, so one body buffer is enough. body_total_ is
  // the Content-Length the buffer belongs to: a receive that failed midway never reaches
  // handleRequest, so the next request checks it against its own before trusting the buffer.
  std::string body_;
  size_t body_total_{0};
  size_t body_received_{0};
  bool body_too_large_{false};
};

}  // namespace esphome::web_climate_editor
