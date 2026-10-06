#include "web_climate_editor.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "esphome/components/climate_hub/entity_lookup.h"
#include "esphome/components/climate_hub/enums.h"
#include "esphome/components/climate_hub/param_table.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::web_climate_editor {

using climate_hub::ClimateConfig;
using climate_hub::ControllerRuntime;
using climate_hub::Result;

static const char *const TAG = "web_climate_editor";
static const char *const NOT_FOUND = "Thermostat not found";
// The hub's own words for the same refusal.
static const char *const STORAGE_UNAVAILABLE = "Thermostat storage is not available";

// clang-format off
static const Route ROUTES[] = {
    {"list", RouteId::LIST, false},
    {"get", RouteId::GET, false},
    {"status", RouteId::STATUS, false},
    {"entities", RouteId::ENTITIES, false},
    {"schema", RouteId::SCHEMA, false},
    {"ping", RouteId::PING, false},
    {"save", RouteId::SAVE, true},
    {"delete", RouteId::DELETE, true},
    {"enable", RouteId::ENABLE, true},
    {"setpoint", RouteId::SETPOINT, true},
};
// clang-format on

// JSON has no NaN: a reading the device does not have is null.
static void set_or_null(JsonObject obj, const char *key, float value) {
  if (std::isnan(value)) {
    obj[key] = nullptr;
  } else {
    obj[key] = value;
  }
}

// What the client sends as String(value), and nothing strtod would take besides: no nan, inf,
// hex or surrounding blanks.
static bool is_decimal(const std::string &text) {
  auto digit = [&text](size_t i) { return i < text.size() && text[i] >= '0' && text[i] <= '9'; };
  size_t i = 0;
  if (i < text.size() && (text[i] == '+' || text[i] == '-'))
    i++;
  size_t digits = 0;
  for (; digit(i); i++)
    digits++;
  if (i < text.size() && text[i] == '.') {
    for (i++; digit(i); i++)
      digits++;
  }
  if (digits == 0)
    return false;
  if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
    i++;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
      i++;
    if (!digit(i))
      return false;
    while (digit(i))
      i++;
  }
  return i == text.size();
}

// `"Living Room" and "Floor"`: the thermostats by the names a person knows them by.
static std::string names_of(const climate_hub::ClimateHub &hub, const std::vector<std::string> &ids) {
  std::string names;
  for (size_t i = 0; i < ids.size(); i++) {
    const ClimateConfig *config = hub.store().get(ids[i]);
    names += std::string(i == 0 ? "" : " and ") + "\"" + (config != nullptr ? config->name : ids[i]) + "\"";
  }
  return names;
}

// What a change did besides itself, for the end of its message: the waiting thermostats that
// started on a relay it freed. The warning, when there is one, comes after it.
static std::string started_note(const climate_hub::ClimateHub &hub, const Result &result) {
  return result.started.empty() ? "" : "; " + names_of(hub, result.started) + " started";
}

static std::string success_json(const std::string &message) {
  JsonDocument doc;
  doc["success"] = true;
  doc["message"] = message;
  std::string json;
  serializeJson(doc, json);
  return json;
}

// web_server_idf knows a handful of codes and turns every other one into a 500. A 405 is
// written by check_method_, which adds the Allow header.
static const char *status_line(int code) {
  switch (code) {
    case 413:
      return "413 Payload Too Large";
    case 503:
      return "503 Service Unavailable";
    case 507:
      return "507 Insufficient Storage";
    default:
      return nullptr;
  }
}

void WebClimateEditor::setup() {
  this->base_->init();
  this->base_->add_handler(&this->guard_);
}

void WebClimateEditor::dump_config() {
  ESP_LOGCONFIG(TAG, "Web Climate Editor:");
  ESP_LOGCONFIG(TAG, "  API: %s/api/", this->url_prefix_.c_str());
}

std::string WebClimateEditor::url_(AsyncWebServerRequest *request) const {
#ifdef USE_ESP32
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  return std::string(request->url_to(buf));
#else
  return request->url();
#endif
}

// The route @p url names, or nullptr. The name is the whole tail: "list/" is not a list.
const Route *WebClimateEditor::route_for_(const std::string &url) const {
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

// The whole prefix, so a name that is no route is this API's 404: unclaimed, the server would
// close the socket without an answer.
bool WebClimateEditor::canHandle(AsyncWebServerRequest *request) const {
  const std::string url = this->url_(request);
  return url == this->url_prefix_ || url.starts_with(this->url_prefix_ + "/");
}

void WebClimateEditor::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                  size_t total) {
  if (index == 0) {
    this->body_.clear();
    this->body_total_ = total;
    this->body_received_ = 0;
    this->body_too_large_ = total > climate_hub::CONFIG_MAX_BYTES;
  }
  this->body_received_ += len;
  if (this->body_too_large_)
    return;
  this->body_.append(reinterpret_cast<const char *>(data), len);
}

void WebClimateEditor::handleRequest(AsyncWebServerRequest *request) {
  const std::string url = this->url_(request);
  // Verbose, not debug: the editor polls the list and the status every few seconds.
  ESP_LOGV(TAG, "Handling request: %s", url.c_str());
  // The buffer is this request's only if it is complete and sized for it.
  if (this->body_total_ != request->contentLength() || this->body_received_ != this->body_total_)
    this->reset_body_();
  const Route *route = this->route_for_(url);
  if (route == nullptr) {
    this->send_error_(request, "Unknown endpoint", 404);
  } else if (this->check_method_(request, *route)) {
    switch (route->id) {
      case RouteId::LIST:
        this->handle_list_(request);
        break;
      case RouteId::GET:
        this->handle_get_(request);
        break;
      case RouteId::STATUS:
        this->handle_status_(request);
        break;
      case RouteId::ENTITIES:
        this->handle_entities_(request);
        break;
      case RouteId::SCHEMA:
        this->handle_schema_(request);
        break;
      case RouteId::PING:
        request->send(200, "application/json", R"({"status":"ok"})");
        break;
      case RouteId::SAVE:
        this->handle_save_(request);
        break;
      case RouteId::DELETE:
        this->handle_delete_(request);
        break;
      case RouteId::ENABLE:
        this->handle_enable_(request);
        break;
      case RouteId::SETPOINT:
        this->handle_setpoint_(request);
        break;
    }
  }
  this->reset_body_();
}

// Releases the buffer rather than keep a request's worth of heap between calls.
void WebClimateEditor::reset_body_() {
  std::string().swap(this->body_);
  this->body_total_ = 0;
  this->body_received_ = 0;
  this->body_too_large_ = false;
}

bool WebClimateEditor::check_method_(AsyncWebServerRequest *request, const Route &route) {
  if (request->method() == (route.mutating ? HTTP_POST : HTTP_GET))
    return true;
  const char *allow = route.mutating ? "POST" : "GET";
  ESP_LOGW(TAG, "Refusing %s on a %s-only route", route.name, allow);
  this->send_status_(request, "405 Method Not Allowed", allow, R"({"success":false,"error":"Method not allowed"})");
  return false;
}

// Present and a slug: an id is a file name, and "Living Room" is a name.
bool WebClimateEditor::read_id_(AsyncWebServerRequest *request, std::string &id) {
  if (!request->hasParam("id")) {
    this->send_error_(request, "Missing id parameter");
    return false;
  }
  id = request->getParam("id")->value();
  if (climate_hub::slugify_id(id) != id) {
    this->send_error_(request, "Invalid id parameter");
    return false;
  }
  return true;
}

// "true" or "false" and nothing else: "1" or "yes" is more likely a typo than a choice.
bool WebClimateEditor::read_bool_(AsyncWebServerRequest *request, const char *name, bool &value) {
  const std::string &text = request->getParam(name)->value();
  value = text == "true";
  if (value || text == "false")
    return true;
  this->send_error_(request, std::string("Invalid ") + name + " parameter");
  return false;
}

bool WebClimateEditor::read_number_(AsyncWebServerRequest *request, float &value) {
  if (!request->hasParam("value")) {
    this->send_error_(request, "Missing value parameter");
    return false;
  }
  const std::string &text = request->getParam("value")->value();
  const double parsed = is_decimal(text) ? strtod(text.c_str(), nullptr) : NAN;
  if (!std::isfinite(parsed)) {
    this->send_error_(request, "Invalid value parameter");
    return false;
  }
  // Out of a float's range the cast is undefined; the hub clamps into the visual range anyway.
  value = static_cast<float>(std::max(-static_cast<double>(FLT_MAX), std::min(static_cast<double>(FLT_MAX), parsed)));
  return true;
}

// --- Reads: each one a single job on the loop task, which owns the documents and the claims ---

void WebClimateEditor::handle_list_(AsyncWebServerRequest *request) {
  std::string json;
  const bool ran = this->hub_->run_on_loop([&]() {
    JsonDocument doc;
    doc["success"] = true;
    doc["count"] = this->hub_->store().size();
    doc["max_controllers"] = this->hub_->max_controllers();
    JsonArray rows = doc["controllers"].to<JsonArray>();
    for (const auto &config : this->hub_->store().all()) {
      JsonObject row = rows.add<JsonObject>();
      row["id"] = config->id;
      row["name"] = config->name;
      row["enabled"] = config->enabled;
      row["kind"] = climate_hub::enums::control_kind_to_string(config->kind);
      row["mode"] = climate_hub::enums::mode_to_string(config->mode);
      row["sensor_id"] = config->sensor_id;
      row["heat_relay_id"] = config->heat.relay_id;
      row["cool_relay_id"] = config->cool.relay_id;
      row["running"] = this->hub_->is_running(config->id);
      // Why it waits, as the Save, the enable or the boot found it, for a client that was not there.
      row["waiting"] = this->hub_->waiting_reason(config->id);
    }
    serializeJson(doc, json);
    return true;
  });
  this->answer_(request, ran, 200, "", json);
}

void WebClimateEditor::handle_get_(AsyncWebServerRequest *request) {
  std::string id;
  if (!this->read_id_(request, id))
    return;
  std::string json;
  std::string error;
  int code = 200;
  const bool ran = this->hub_->run_on_loop([&]() {
    const ClimateConfig *config = this->hub_->store().get(id);
    if (config == nullptr) {
      code = 404;
      error = NOT_FOUND;
      return true;
    }
    // The bare document, not an envelope: what save takes back.
    JsonDocument doc;
    config->serialize(doc.to<JsonObject>());
    serializeJson(doc, json);
    return true;
  });
  this->answer_(request, ran, code, error, json);
}

void WebClimateEditor::handle_status_(AsyncWebServerRequest *request) {
  std::string only;
  if (request->hasParam("id") && !this->read_id_(request, only))
    return;
  std::string json;
  std::string error;
  int code = 200;
  const bool ran = this->hub_->run_on_loop([&]() {
    if (!only.empty() && this->hub_->store().get(only) == nullptr) {
      code = 404;
      error = NOT_FOUND;
      return true;
    }
    const uint32_t now = this->hub_->now_ms();
    JsonDocument doc;
    doc["success"] = true;
    JsonArray rows = doc["controllers"].to<JsonArray>();
    for (const auto &config : this->hub_->store().all()) {
      if (!only.empty() && config->id != only)
        continue;
      const ControllerRuntime *runtime = this->hub_->runtime(config->id);
      JsonObject row = rows.add<JsonObject>();
      row["id"] = config->id;
      row["running"] = runtime != nullptr;
      row["waiting"] = this->hub_->waiting_reason(config->id);
      row["action"] =
          climate_hub::enums::action_to_string(runtime != nullptr ? runtime->action() : climate_hub::HubAction::OFF);
      row["fault"] =
          climate_hub::enums::fault_to_string(runtime != nullptr ? runtime->fault() : climate_hub::HubFault::NONE);
      // A stopped thermostat still reports its room, read off the sensor itself.
      set_or_null(
          row, "current_temperature",
          runtime != nullptr ? runtime->entity()->current_temperature : this->hub_->sensor_reading(config->sensor_id));
      set_or_null(row, "sensor_age_s", runtime != nullptr ? runtime->sensor_age_s(now) : NAN);
      row["setpoint"] = config->setpoint;
      row["min_temperature"] = config->visual.min_temperature;
      row["max_temperature"] = config->visual.max_temperature;
      row["step"] = config->visual.step;
      if (config->kind == climate_hub::ControlKind::BANG_BANG) {
        row["switch_low"] = config->switch_low();
        row["switch_high"] = config->switch_high();
      }
      row["heat_duty"] = runtime != nullptr ? runtime->heat_duty() : 0.f;
      row["cool_duty"] = runtime != nullptr ? runtime->cool_duty() : 0.f;
      row["heat_relay_on"] = runtime != nullptr && runtime->heat_relay_on();
      row["cool_relay_on"] = runtime != nullptr && runtime->cool_relay_on();
      if (runtime != nullptr && config->kind == climate_hub::ControlKind::PID) {
        JsonObject pid = row["pid"].to<JsonObject>();
        set_or_null(pid, "error", runtime->pid().error());
        set_or_null(pid, "proportional", runtime->pid().proportional_term());
        set_or_null(pid, "integral", runtime->pid().integral_term());
        set_or_null(pid, "derivative", runtime->pid().derivative_term());
        // Without it a tuning panel reads the scaled terms as a PID that lost its gains.
        pid["in_deadband"] = runtime->pid().in_deadband();
      }
    }
    serializeJson(doc, json);
    return true;
  });
  this->answer_(request, ran, code, error, json);
}

// A job too: which relay a running thermostat holds is the loop task's to know.
void WebClimateEditor::handle_entities_(AsyncWebServerRequest *request) {
  std::string json;
  const bool ran = this->hub_->run_on_loop([&]() {
    JsonDocument doc;
    doc["success"] = true;
    JsonArray sensors = doc["sensors"].to<JsonArray>();
    JsonArray switches = doc["switches"].to<JsonArray>();
#ifdef USE_SENSOR
    for (auto *entity : App.get_sensors()) {
      // Only what a thermostat may run on: the hub refuses the rest.
      if (entity->is_internal() || !climate_hub::reports_celsius(*entity))
        continue;
      JsonObject row = sensors.add<JsonObject>();
      row["object_id"] = climate_hub::object_id_of(*entity);
      row["name"] = entity->get_name().c_str();
      row["unit"] = entity->get_unit_of_measurement_ref().c_str();
    }
#endif
#ifdef USE_SWITCH
    for (auto *entity : App.get_switches()) {
      if (entity->is_internal())
        continue;
      const std::string object_id = climate_hub::object_id_of(*entity);
      JsonObject row = switches.add<JsonObject>();
      row["object_id"] = object_id;
      row["name"] = entity->get_name().c_str();
      row["claimed_by"] = this->hub_->claimed_by(object_id);
    }
#endif
    serializeJson(doc, json);
    return true;
  });
  this->answer_(request, ran, 200, "", json);
}

// Build-time data only, so it stays on the server task: the codec clamps against this very
// table, and the form it draws cannot offer what the device would change.
void WebClimateEditor::handle_schema_(AsyncWebServerRequest *request) {
  JsonDocument doc;
  JsonArray kinds = doc["kinds"].to<JsonArray>();
  for (auto kind : {climate_hub::ControlKind::PID, climate_hub::ControlKind::BANG_BANG})
    kinds.add(climate_hub::enums::control_kind_to_string(kind));
  JsonArray modes = doc["modes"].to<JsonArray>();
  for (auto mode : {climate_hub::HubMode::OFF, climate_hub::HubMode::HEAT, climate_hub::HubMode::COOL,
                    climate_hub::HubMode::HEAT_COOL})
    modes.add(climate_hub::enums::mode_to_string(mode));
  JsonArray faults = doc["faults"].to<JsonArray>();
  for (auto fault : {climate_hub::HubFault::NONE, climate_hub::HubFault::SENSOR_STALE, climate_hub::HubFault::OVERTEMP})
    faults.add(climate_hub::enums::fault_to_string(fault));
  doc["max_controllers"] = this->hub_->max_controllers();
  doc["name_max_length"] = climate_hub::NAME_MAX_LENGTH;
  JsonObject params = doc["params"].to<JsonObject>();
  for (size_t i = 0; i < climate_hub::PARAM_COUNT; i++) {
    const climate_hub::ParamDesc &param = climate_hub::PARAMS[i];
    JsonArray group =
        params[param.group].is<JsonArray>() ? params[param.group].as<JsonArray>() : params[param.group].to<JsonArray>();
    JsonObject entry = group.add<JsonObject>();
    entry["key"] = param.key;
    entry["label"] = param.label;
    entry["unit"] = param.unit;
    entry["group"] = param.group;
    entry["def"] = param.def;
    entry["min"] = param.min;
    entry["max"] = param.max;
    entry["step"] = param.step;
    entry["integer"] = param.integer;
    entry["kind"] = param.kind;
    entry["hint"] = param.hint;
  }
  std::string json;
  serializeJson(doc, json);
  request->send(200, "application/json", json.c_str());
}

// --- Writes: the checks and the change go over together, so the answer is what happened ---

void WebClimateEditor::handle_save_(AsyncWebServerRequest *request) {
  if (this->body_too_large_) {
    this->send_error_(request, "Request body over 8 KiB", 413);
    return;
  }
  if (this->body_.empty()) {
    this->send_error_(request, "Empty request body");
    return;
  }
  // A failed hub runs a job in place, on this task's small stack, only to refuse the write:
  // answer before parsing. The hub fails only in its setup, before the server starts.
  if (this->hub_->is_failed()) {
    this->send_error_(request, STORAGE_UNAVAILABLE, 500);
    return;
  }
  std::string json;
  std::string error;
  int code = 400;
  // Parsed on the loop task too: its stack has the room, the server task's has little.
  const bool ran = this->hub_->run_on_loop([&]() {
    JsonDocument doc;
    const DeserializationError parsed = deserializeJson(doc, this->body_);
    if (parsed) {
      error = std::string("JSON parse error: ") + parsed.c_str();
      return true;
    }
    ClimateConfig config;
    if (!config.deserialize(doc.as<JsonObject>(), false, &error))
      return true;
    // An id picks the thermostat to replace; none, or "", creates one.
    const std::string id = config.id;
    const Result result = id.empty() ? this->hub_->create(config) : this->hub_->update(id, config);
    if (!result.ok) {
      code = result.code;
      error = result.error;
      return true;
    }
    std::string message = id.empty() ? "Thermostat created" : "Thermostat updated";
    message += started_note(*this->hub_, result);
    if (!result.warning.empty())
      message += "; " + result.warning;
    JsonDocument answer;
    answer["success"] = true;
    answer["message"] = message;
    answer["id"] = id.empty() ? result.id : id;
    // Saved enabled but not running: a client should not have to parse the message for that.
    if (!result.warning.empty())
      answer["warning"] = result.warning;
    serializeJson(answer, json);
    return true;
  });
  this->answer_(request, ran, code, error, json);
}

void WebClimateEditor::handle_delete_(AsyncWebServerRequest *request) {
  std::string id;
  if (!this->read_id_(request, id))
    return;
  std::string json;
  std::string error;
  int code = 400;
  const bool ran = this->hub_->run_on_loop([&]() {
    const Result result = this->hub_->remove(id);
    if (!result.ok) {
      code = result.code;
      error = result.error;
      return true;
    }
    JsonDocument answer;
    answer["success"] = true;
    // The hub blanks a file it cannot unlink; this is the rarer case where that failed too.
    answer["message"] = std::string(result.persisted ? "Thermostat deleted"
                                                     : "Thermostat deleted; its file could not be removed, so it "
                                                       "comes back at the next boot") +
                        started_note(*this->hub_, result);
    // As on /enable, so a client need not parse the message for it.
    answer["persisted"] = result.persisted;
    serializeJson(answer, json);
    return true;
  });
  this->answer_(request, ran, code, error, json);
}

void WebClimateEditor::handle_enable_(AsyncWebServerRequest *request) {
  std::string id;
  if (!this->read_id_(request, id))
    return;
  if (!request->hasParam("value")) {
    this->send_error_(request, "Missing value parameter");
    return;
  }
  bool enabled = false;
  bool take_over = false;
  if (!this->read_bool_(request, "value", enabled))
    return;
  if (request->hasParam("take_over") && !this->read_bool_(request, "take_over", take_over))
    return;
  std::string json;
  std::string error;
  int code = 400;
  const bool ran = this->hub_->run_on_loop([&]() {
    const Result result = this->hub_->set_enabled(id, enabled, take_over);
    if (!result.ok) {
      code = result.code;
      error = result.error;
      return true;
    }
    std::string message = enabled ? "Thermostat enabled" : "Thermostat disabled";
    // Who a take-over stored disabled, then who started on a relay it freed.
    if (!result.stopped.empty())
      message += "; " + names_of(*this->hub_, result.stopped) + " stopped";
    message += started_note(*this->hub_, result);
    if (!result.warning.empty())
      message += "; " + result.warning;
    JsonDocument answer;
    answer["success"] = true;
    answer["message"] = message;
    answer["persisted"] = result.persisted;
    // Stored enabled but not running, as a Save answers it.
    if (!result.warning.empty())
      answer["warning"] = result.warning;
    serializeJson(answer, json);
    return true;
  });
  this->answer_(request, ran, code, error, json);
}

// Running or not: a stopped thermostat has no entity to set a target through.
void WebClimateEditor::handle_setpoint_(AsyncWebServerRequest *request) {
  std::string id;
  float value = NAN;
  if (!this->read_id_(request, id) || !this->read_number_(request, value))
    return;
  std::string json;
  std::string error;
  int code = 400;
  const bool ran = this->hub_->run_on_loop([&]() {
    const Result result = this->hub_->set_setpoint(id, value);
    if (!result.ok) {
      code = result.code;
      error = result.error;
      return true;
    }
    json = success_json("Setpoint updated");
    return true;
  });
  this->answer_(request, ran, code, error, json);
}

// --- Answers ---

void WebClimateEditor::answer_(AsyncWebServerRequest *request, bool ran, int code, const std::string &error,
                               const std::string &json) {
  if (!ran) {
    // Nothing was read or written, so the same call can simply be made again.
    this->send_error_(request, "Device busy", 503);
  } else if (!error.empty()) {
    this->send_error_(request, error, code);
  } else {
    this->send_json_(request, 200, json);
  }
}

void WebClimateEditor::send_json_(AsyncWebServerRequest *request, int code, const std::string &json) {
  const char *status = status_line(code);
  if (status == nullptr) {
    request->send(code, "application/json", json.c_str());
  } else {
    this->send_status_(request, status, nullptr, json.c_str());
  }
}

void WebClimateEditor::send_error_(AsyncWebServerRequest *request, const std::string &message, int code) {
  JsonDocument doc;
  doc["success"] = false;
  doc["error"] = message;
  std::string json;
  serializeJson(doc, json);
  this->send_json_(request, code, json);
}

void WebClimateEditor::send_status_(AsyncWebServerRequest *request, const char *status, const char *allow,
                                    const char *body) {
#ifdef USE_ESP32
  // By hand because AsyncWebServerRequest::send() turns every status it does not know into a
  // 500, and because httpd_resp_set_hdr() keeps the pointer it is given rather than a copy:
  // the status and the header are literals, and the body is sent before this returns.
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  if (allow != nullptr)
    httpd_resp_set_hdr(req, "Allow", allow);
  httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
#else
  auto *response =
      request->beginResponse(atoi(status), "application/json", reinterpret_cast<const uint8_t *>(body), strlen(body));
  if (allow != nullptr)
    response->addHeader("Allow", allow);
  request->send(response);
#endif
}

}  // namespace esphome::web_climate_editor
