#include "web_automation_editor.h"
#include <ArduinoJson.h>
#include <cstdint>
#include "esphome/components/automations/entity_lookup.h"
#include "esphome/components/automations/enums.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::web_automation_editor {

static const char *const TAG = "web_automation_editor";
// The same ceiling the engine puts on a rule file.
static const size_t MAX_BODY_BYTES = 16384;

// clang-format off
static const Route ROUTES[] = {
    {"list", RouteId::LIST, false},
    {"get", RouteId::GET, false},
    {"save", RouteId::SAVE, true},
    {"delete", RouteId::DELETE, true},
    {"export", RouteId::EXPORT, false},
    {"entities", RouteId::ENTITIES, false},
    {"schema", RouteId::SCHEMA, false},
    {"reboot", RouteId::REBOOT, true},
    {"ping", RouteId::PING, false},
};
// clang-format on

// What the editor may offer; the engine's parsers are the authority on each word.
static const char *const SCHEMA = R"({
  "triggers": [
    {"type": "input", "subtypes": ["press", "release", "click", "state_change"]},
    {"type": "temperature", "subtypes": ["above", "below", "range"]},
    {"type": "cron", "subtypes": []},
    {"type": "startup", "subtypes": []},
    {"type": "switch", "subtypes": ["turn_on", "turn_off", "state_change"]}
  ],
  "conditions": [
    {"type": "input"},
    {"type": "temperature", "subtypes": ["above", "below", "range"]},
    {"type": "and"},
    {"type": "or"},
    {"type": "xor"}
  ],
  "actions": [
    {"type": "switch", "subtypes": ["turn_on", "turn_off", "toggle", "follow"]},
    {"type": "delay"}
  ],
  "cron_presets": ["daily", "hourly", "every_n_minutes", "weekly", "monthly", "custom"]
})";

static std::string config_json(const automations::AutomationConfig &config) {
  JsonDocument doc;
  JsonObject obj = doc.to<JsonObject>();
  config.serialize(obj);
  std::string json;
  serializeJson(doc, json);
  return json;
}

void WebAutomationEditor::setup() {
  this->base_->init();
  this->base_->add_handler(this);
}

void WebAutomationEditor::dump_config() {
  ESP_LOGCONFIG(TAG, "Web Automation Editor:");
  ESP_LOGCONFIG(TAG, "  API: %s/api/", this->url_prefix_.c_str());
}

std::string WebAutomationEditor::url_(AsyncWebServerRequest *request) const {
#ifdef USE_ESP32
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  return std::string(request->url_to(buf));
#else
  return request->url();
#endif
}

// The route @p url names, or nullptr. The name is the whole tail: "list/" is not a list.
const Route *WebAutomationEditor::route_for_(const std::string &url) const {
  const std::string api = this->url_prefix_ + "/api/";
  if (!url.starts_with(api))
    return nullptr;
  const std::string tail = url.substr(api.size());
  for (const Route &route : ROUTES) {
    if (tail == route.name)
      return &route;
  }
  return nullptr;
}

bool WebAutomationEditor::canHandle(AsyncWebServerRequest *request) const {
  return this->route_for_(this->url_(request)) != nullptr;
}

void WebAutomationEditor::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
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

void WebAutomationEditor::handleRequest(AsyncWebServerRequest *request) {
  const std::string url = this->url_(request);
  const Route *route = this->route_for_(url);
  ESP_LOGD(TAG, "%s", url.c_str());
  // The buffer is this request's only if it is complete and sized for it.
  if (this->body_total_ != request->contentLength() || this->body_received_ != this->body_total_)
    this->reset_body_();
  if (route != nullptr && this->check_method_(request, *route)) {
    switch (route->id) {
      case RouteId::LIST:
        this->handle_list_(request);
        break;
      case RouteId::GET:
        this->handle_get_(request);
        break;
      case RouteId::SAVE:
        this->handle_save_(request);
        break;
      case RouteId::DELETE:
        this->handle_delete_(request);
        break;
      case RouteId::EXPORT:
        this->handle_export_(request);
        break;
      case RouteId::ENTITIES:
        this->handle_entities_(request);
        break;
      case RouteId::SCHEMA:
        this->handle_schema_(request);
        break;
      case RouteId::REBOOT:
        this->send_success_(request, "Rebooting device...");
        this->reboot_();
        break;
      case RouteId::PING:
        request->send(200, "application/json", R"({"status":"ok"})");
        break;
    }
  }
  this->reset_body_();
}

// Releases the buffer rather than keep a request's worth of heap between calls.
void WebAutomationEditor::reset_body_() {
  std::string().swap(this->body_);
  this->body_total_ = 0;
  this->body_received_ = 0;
  this->body_too_large_ = false;
}

// The whole parameter, decimal, non-zero, within uint32_t: "1junk" is not rule 1.
bool WebAutomationEditor::read_id_(AsyncWebServerRequest *request, uint32_t &id) {
  if (!request->hasParam("id")) {
    this->send_error_(request, "Missing id parameter");
    return false;
  }
  const std::string &value = request->getParam("id")->value();
  uint64_t parsed = 0;
  bool valid = !value.empty() && value.size() <= 10;
  for (size_t i = 0; valid && i < value.size(); i++) {
    valid = value[i] >= '0' && value[i] <= '9';
    parsed = parsed * 10 + (value[i] - '0');
  }
  if (!valid || parsed == 0 || parsed > UINT32_MAX) {
    this->send_error_(request, "Invalid id parameter");
    return false;
  }
  id = static_cast<uint32_t>(parsed);
  return true;
}

bool WebAutomationEditor::check_method_(AsyncWebServerRequest *request, const Route &route) {
  if (request->method() == (route.mutating ? HTTP_POST : HTTP_GET))
    return true;
  const char *allow = route.mutating ? "POST" : "GET";
  ESP_LOGW(TAG, "Refusing %s on a %s-only route", route.name, allow);
  this->send_status_(request, "405 Method Not Allowed", allow, R"({"success":false,"error":"Method not allowed"})");
  return false;
}

void WebAutomationEditor::handle_list_(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray rows = doc["automations"].to<JsonArray>();
  for (const auto &config : this->storage_->configs().get_all_configs()) {
    JsonObject row = rows.add<JsonObject>();
    row["id"] = config.id;
    row["name"] = config.name;
    row["enabled"] = config.enabled;
    row["trigger_count"] = config.triggers.size();
    row["action_count"] = config.actions.size();
    row["else_action_count"] = config.else_actions.size();
    row["mode"] = automations::EnumUtils::automation_mode_to_string(config.mode);
  }
  std::string json;
  serializeJson(doc, json);
  request->send(200, "application/json", json.c_str());
}

const automations::AutomationConfig *WebAutomationEditor::find_(uint32_t id) const {
  for (const auto &config : this->storage_->configs().get_all_configs()) {
    if (config.id == id)
      return &config;
  }
  return nullptr;
}

void WebAutomationEditor::handle_get_(AsyncWebServerRequest *request) {
  uint32_t id;
  if (!this->read_id_(request, id))
    return;
  const automations::AutomationConfig *config = this->find_(id);
  if (config == nullptr) {
    this->send_error_(request, "Automation not found", 404);
    return;
  }
  request->send(200, "application/json", config_json(*config).c_str());
}

void WebAutomationEditor::handle_save_(AsyncWebServerRequest *request) {
  if (this->body_too_large_) {
    this->send_status_(request, "413 Payload Too Large", nullptr,
                       R"({"success":false,"error":"Request body over 16 KiB"})");
    return;
  }
  if (this->body_.empty()) {
    this->send_error_(request, "Empty request body");
    return;
  }
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, this->body_);
  if (error) {
    this->send_error_(request, std::string("JSON parse error: ") + error.c_str());
    return;
  }
  automations::AutomationConfig config;
  if (!config.deserialize(doc.as<JsonObject>())) {
    this->send_error_(request, "Failed to parse automation config");
    return;
  }
  // The engine refuses a taken name too; this is here to answer with the reason.
  if (this->storage_->is_name_taken(config.name, config.id)) {
    this->send_error_(request, "An automation named \"" + config.name + "\" already exists");
    return;
  }
  if (config.id > 0) {
    if (this->find_(config.id) == nullptr) {
      this->send_error_(request, "Automation not found", 404);
      return;
    }
    if (!this->storage_->update_automation(config.id, config)) {
      this->send_error_(request, "Failed to update automation");
      return;
    }
    this->send_success_(request, "Automation updated");
    return;
  }
  const uint32_t assigned = this->storage_->add_automation(config);
  if (assigned == 0) {
    this->send_error_(request, "Failed to create automation");
    return;
  }
  this->send_success_(request, "Automation created", assigned);
}

void WebAutomationEditor::handle_delete_(AsyncWebServerRequest *request) {
  uint32_t id;
  if (!this->read_id_(request, id))
    return;
  if (this->find_(id) == nullptr) {
    this->send_error_(request, "Automation not found", 404);
    return;
  }
  if (!this->storage_->remove_automation(id)) {
    this->send_error_(request, "Failed to delete automation");
    return;
  }
  this->send_success_(request, "Automation deleted");
}

// Every rule in its stored form, one document at a time: the peak is one rule plus the
// response, not every rule twice.
void WebAutomationEditor::handle_export_(AsyncWebServerRequest *request) {
  std::string json = R"({"version":1,"automations":[)";
  bool first = true;
  for (const auto &config : this->storage_->configs().get_all_configs()) {
    if (!first)
      json += ',';
    first = false;
    json += config_json(config);
  }
  json += "]}";
  request->send(200, "application/json", json.c_str());
}

void WebAutomationEditor::handle_entities_(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray binary_sensors = doc["binary_sensors"].to<JsonArray>();
  JsonArray sensors = doc["sensors"].to<JsonArray>();
  JsonArray switches = doc["switches"].to<JsonArray>();
#ifdef USE_BINARY_SENSOR
  for (auto *entity : App.get_binary_sensors()) {
    if (entity->is_internal())
      continue;
    JsonObject row = binary_sensors.add<JsonObject>();
    row["object_id"] = automations::object_id_of(*entity);
    row["name"] = entity->get_name().c_str();
  }
#endif
#ifdef USE_SENSOR
  for (auto *entity : App.get_sensors()) {
    if (entity->is_internal())
      continue;
    JsonObject row = sensors.add<JsonObject>();
    row["object_id"] = automations::object_id_of(*entity);
    row["name"] = entity->get_name().c_str();
    row["unit"] = entity->get_unit_of_measurement_ref().c_str();
  }
#endif
#ifdef USE_SWITCH
  for (auto *entity : App.get_switches()) {
    if (entity->is_internal())
      continue;
    JsonObject row = switches.add<JsonObject>();
    row["object_id"] = automations::object_id_of(*entity);
    row["name"] = entity->get_name().c_str();
  }
#endif
  std::string json;
  serializeJson(doc, json);
  request->send(200, "application/json", json.c_str());
}

void WebAutomationEditor::handle_schema_(AsyncWebServerRequest *request) {
  request->send(200, "application/json", SCHEMA);
}

void WebAutomationEditor::send_error_(AsyncWebServerRequest *request, const std::string &message, int code) {
  JsonDocument doc;
  doc["success"] = false;
  doc["error"] = message;
  std::string json;
  serializeJson(doc, json);
  request->send(code, "application/json", json.c_str());
}

void WebAutomationEditor::send_success_(AsyncWebServerRequest *request, const char *message, uint32_t id) {
  JsonDocument doc;
  doc["success"] = true;
  doc["message"] = message;
  if (id != 0)
    doc["id"] = id;
  std::string json;
  serializeJson(doc, json);
  request->send(200, "application/json", json.c_str());
}

void WebAutomationEditor::send_status_(AsyncWebServerRequest *request, const char *status, const char *allow,
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

// On the loop task, as upstream's web OTA does: the teardown must not race the loop.
void WebAutomationEditor::reboot_() {
  this->set_timeout(100, []() { App.safe_reboot(); });
}

}  // namespace esphome::web_automation_editor
