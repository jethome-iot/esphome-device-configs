#include "web_schedule_editor.h"
#include <ArduinoJson.h>
#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <vector>
#include "esphome/components/schedules/schedule_config.h"
#include "esphome/components/schedules/week_clock.h"
#include "esphome/core/log.h"

namespace esphome::web_schedule_editor {

using schedules::PointRef;
using schedules::Result;
using schedules::ScheduleConfig;

static const char *const TAG = "web_schedule_editor";
static const char *const NOT_FOUND = "Schedule not found";
// The engine's own words for the same refusal.
static const char *const STORAGE_UNAVAILABLE = "Schedule storage is not available";
// A read whose answer the heap could not hold: nothing changed, and the heap moves.
static const char *const LOW_MEMORY = "Device is low on memory; try again";

static_assert(schedules::MAX_FILE_BYTES == 16384, "the body's 413 names the limit");

// clang-format off
static const Route ROUTES[] = {
    {"list", RouteId::LIST, false},
    {"get", RouteId::GET, false},
    {"targets", RouteId::TARGETS, false},
    {"schema", RouteId::SCHEMA, false},
    {"ping", RouteId::PING, false},
    {"save", RouteId::SAVE, true},
    {"import", RouteId::IMPORT, true},
    {"delete", RouteId::DELETE, true},
    {"enable", RouteId::ENABLE, true},
};
// clang-format on

// JSON has no NaN.
static void set_or_null(JsonObject obj, const char *key, float value) {
  if (std::isnan(value)) {
    obj[key] = nullptr;
  } else {
    obj[key] = value;
  }
}

// `"Winter" and "Porch"`: the schedules by the names a person knows them by.
static std::string names_of(const schedules::ScheduleEngine &engine, const std::vector<std::string> &ids) {
  std::string names;
  for (size_t i = 0; i < ids.size(); i++) {
    const ScheduleConfig *config = engine.get(ids[i]);
    names += std::string(i == 0 ? "" : " and ") + "\"" + (config != nullptr ? config->name : ids[i]) + "\"";
  }
  return names;
}

// What a change did besides itself, for the end of its message: the schedules that waited for
// a target it let go and drive theirs now. The warning, when there is one, comes after it.
static std::string started_note(const schedules::ScheduleEngine &engine, const Result &result) {
  return result.started.empty() ? "" : "; " + names_of(engine, result.started) + " started";
}

static void add_ids(JsonObject obj, const char *key, const std::vector<std::string> &ids) {
  if (ids.empty())
    return;
  JsonArray list = obj[key].to<JsonArray>();
  for (const std::string &id : ids)
    list.add(id);
}

// {"day": "wed", "at": "08:30", "mode": "day"}, or null while the clock is not valid.
static void set_point(JsonObject obj, const char *key, const ScheduleConfig &config, const PointRef *ref) {
  if (ref == nullptr) {
    obj[key] = nullptr;
    return;
  }
  JsonObject point = obj[key].to<JsonObject>();
  point["day"] = schedules::DAY_NAMES[ref->day];
  point["at"] = schedules::format_hhmm(ref->minute);
  point["mode"] = config.modes[ref->point->mode].id;
}

// ArduinoJson's writer that sends the text a buffer at a time, so no answer is ever one block
// of its own size. On the host the stand-in's stream takes the chunks.
class ChunkWriter {
 public:
  ChunkWriter(AsyncWebServerRequest *request, char *buffer, size_t size)
      : request_(request), buffer_(buffer), size_(size) {
#ifdef USE_ESP32
    httpd_resp_set_type(*request, "application/json");
#else
    this->stream_ = request->beginResponseStream("application/json");
#endif
  }
  size_t write(uint8_t c) { return this->write(&c, 1); }
  size_t write(const uint8_t *data, size_t length) {
    for (size_t done = 0; done < length;) {
      const size_t n = std::min(length - done, this->size_ - this->used_);
      memcpy(this->buffer_ + this->used_, data + done, n);
      this->used_ += n;
      done += n;
      if (this->used_ == this->size_)
        this->flush_();
    }
    return length;
  }
  void finish() {
    this->flush_();
#ifdef USE_ESP32
    // Ends the body even after a chunk failed: the client sees it cut short, not hanging.
    httpd_resp_send_chunk(*this->request_, nullptr, 0);
#else
    this->request_->send(this->stream_);
#endif
  }

 protected:
  void flush_() {
#ifdef USE_ESP32
    if (this->used_ > 0 && !this->failed_)
      this->failed_ = httpd_resp_send_chunk(*this->request_, this->buffer_, this->used_) != ESP_OK;
#else
    this->stream_->print(std::string(this->buffer_, this->used_));
#endif
    this->used_ = 0;
  }
  AsyncWebServerRequest *request_;
  char *buffer_;
  size_t size_;
  size_t used_{0};
#ifdef USE_ESP32
  bool failed_{false};
#else
  AsyncResponseStream *stream_;
#endif
};

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

void WebScheduleEditor::setup() {
  this->base_->init();
  this->base_->add_handler(&this->guard_);
}

void WebScheduleEditor::dump_config() {
  ESP_LOGCONFIG(TAG, "Web Schedule Editor:");
  ESP_LOGCONFIG(TAG, "  API: %s/api/", this->url_prefix_.c_str());
}

std::string WebScheduleEditor::url_(AsyncWebServerRequest *request) const {
#ifdef USE_ESP32
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  return std::string(request->url_to(buf));
#else
  return request->url();
#endif
}

// The route @p url names, or nullptr. The name is the whole tail: "list/" is not a list.
const Route *WebScheduleEditor::route_for_(const std::string &url) const {
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
bool WebScheduleEditor::canHandle(AsyncWebServerRequest *request) const {
  const std::string url = this->url_(request);
  return url == this->url_prefix_ || url.starts_with(this->url_prefix_ + "/");
}

void WebScheduleEditor::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                   size_t total) {
  if (index == 0) {
    this->reset_body_();
    this->body_total_ = total;
    this->body_too_large_ = total > schedules::MAX_FILE_BYTES;
    // All of it at once, or none: growing a string chunk by chunk aborts when the heap runs short.
    if (!this->body_too_large_ && total > 0) {
      this->body_ = static_cast<char *>(this->json_allocator_()->allocate(total));
      this->body_no_memory_ = this->body_ == nullptr;
    }
  }
  if (this->body_ != nullptr && this->body_received_ + len <= this->body_total_)
    memcpy(this->body_ + this->body_received_, data, len);
  this->body_received_ += len;
}

void WebScheduleEditor::handleRequest(AsyncWebServerRequest *request) {
  const std::string url = this->url_(request);
  // Verbose, not debug: the editor polls the list every few seconds.
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
      case RouteId::TARGETS:
        this->handle_targets_(request);
        break;
      case RouteId::SCHEMA:
        this->handle_schema_(request);
        break;
      case RouteId::PING:
        request->send(200, "application/json", R"({"status":"ok"})");
        break;
      case RouteId::SAVE:
        this->handle_document_(request, false);
        break;
      case RouteId::IMPORT:
        this->handle_document_(request, true);
        break;
      case RouteId::DELETE:
        this->handle_delete_(request);
        break;
      case RouteId::ENABLE:
        this->handle_enable_(request);
        break;
    }
  }
  this->reset_body_();
}

// Releases the buffer rather than keep a request's worth of heap between calls.
void WebScheduleEditor::reset_body_() {
  if (this->body_ != nullptr)
    this->json_allocator_()->deallocate(this->body_);
  this->body_ = nullptr;
  this->body_total_ = 0;
  this->body_received_ = 0;
  this->body_too_large_ = false;
  this->body_no_memory_ = false;
}

bool WebScheduleEditor::check_method_(AsyncWebServerRequest *request, const Route &route) {
  if (request->method() == (route.mutating ? HTTP_POST : HTTP_GET))
    return true;
  const char *allow = route.mutating ? "POST" : "GET";
  ESP_LOGW(TAG, "Refusing %s on a %s-only route", route.name, allow);
  this->send_status_(request, "405 Method Not Allowed", allow, R"({"success":false,"error":"Method not allowed"})");
  return false;
}

// Present and a slug: an id is a file name, and "Porch light" is a name.
bool WebScheduleEditor::read_id_(AsyncWebServerRequest *request, std::string &id) {
  if (!request->hasParam("id")) {
    this->send_error_(request, "Missing id parameter");
    return false;
  }
  id = request->getParam("id")->value();
  if (schedules::slugify_id(id) != id) {
    this->send_error_(request, "Invalid id parameter");
    return false;
  }
  return true;
}

// "true" or "false" and nothing else: "1" or "yes" is more likely a typo than a choice.
bool WebScheduleEditor::read_bool_(AsyncWebServerRequest *request, const char *name, bool &value) {
  const std::string &text = request->getParam(name)->value();
  value = text == "true";
  if (value || text == "false")
    return true;
  this->send_error_(request, std::string("Invalid ") + name + " parameter");
  return false;
}

// --- Reads: each one a single job on the loop task, which owns the schedules ---

void WebScheduleEditor::handle_list_(AsyncWebServerRequest *request) {
  JsonDocument doc(this->json_allocator_());
  const bool ran = this->engine_->run_on_loop([&]() {
    const std::vector<const ScheduleConfig *> all = this->engine_->schedules();
    doc["success"] = true;
    doc["count"] = all.size();
    doc["max_schedules"] = this->engine_->max_schedules();
    // The device's own day and time: the page draws "now" from it, not from the browser's zone.
    std::string day;
    std::string time;
    if (this->engine_->clock_text(&day, &time)) {
      JsonObject clock = doc["clock"].to<JsonObject>();
      clock["day"] = day;
      clock["time"] = time;
    } else {
      doc["clock"] = nullptr;
    }
    JsonArray rows = doc["schedules"].to<JsonArray>();
    for (const ScheduleConfig *config : all) {
      JsonObject row = rows.add<JsonObject>();
      row["id"] = config->id;
      row["revision"] = config->revision;
      row["name"] = config->name;
      row["enabled"] = config->enabled;
      // Each relay and thermostat the modes name, once, by the name a person knows it by.
      JsonArray targets = row["targets"].to<JsonArray>();
      for (const schedules::Target &target : config->targets) {
        JsonObject item = targets.add<JsonObject>();
        item["type"] = schedules::target_type_to_string(target.type);
        item["id"] = target.id;
        item["name"] = this->engine_->target_name(target);
      }
      const schedules::ScheduleStatus status = this->engine_->status(config->id);
      JsonObject state = row["status"].to<JsonObject>();
      state["state"] = schedules::state_to_string(status.state);
      state["reason"] = status.reason;
      if (!status.note.empty())
        state["note"] = status.note;
      JsonArray issues = state["issues"].to<JsonArray>();
      for (const schedules::Issue &issue : status.issues) {
        JsonObject item = issues.add<JsonObject>();
        item["mode"] = config->modes[issue.mode].id;
        item["action"] = issue.action;
        item["kind"] = schedules::issue_kind_to_string(issue.kind);
        item["text"] = issue.text;
        if (issue.kind == schedules::IssueKind::HELD)
          item["held_by"] = issue.held_by;
      }
      // A disabled schedule too: the list previews what it would do.
      PointRef now;
      PointRef next;
      const bool placed = this->engine_->now_and_next(*config, &now, &next);
      set_point(row, "now", *config, placed ? &now : nullptr);
      set_point(row, "next", *config, placed ? &next : nullptr);
    }
    return true;
  });
  this->answer_(request, ran, 200, "", doc);
}

void WebScheduleEditor::handle_get_(AsyncWebServerRequest *request) {
  std::string id;
  if (!this->read_id_(request, id))
    return;
  JsonDocument doc(this->json_allocator_());
  std::string error;
  int code = 200;
  const bool ran = this->engine_->run_on_loop([&]() {
    // The bare document, not an envelope: what save and import take back.
    if (!this->engine_->document(id, doc)) {
      code = 404;
      error = NOT_FOUND;
    }
    return true;
  });
  this->answer_(request, ran, code, error, doc);
}

// A job too: who holds a relay and which thermostats there are is the loop task's to know.
void WebScheduleEditor::handle_targets_(AsyncWebServerRequest *request) {
  JsonDocument doc(this->json_allocator_());
  const bool ran = this->engine_->run_on_loop([&]() {
    doc["success"] = true;
    JsonArray relays = doc["relays"].to<JsonArray>();
    for (const schedules::RelayTarget &relay : this->engine_->relay_targets()) {
      JsonObject row = relays.add<JsonObject>();
      row["object_id"] = relay.object_id;
      row["name"] = relay.name;
      row["held_by"] = relay.held_by;
      row["scheduled_by"] = relay.scheduled_by;
    }
    JsonArray thermostats = doc["thermostats"].to<JsonArray>();
    for (const schedules::ThermostatTarget &thermostat : this->engine_->thermostat_targets()) {
      JsonObject row = thermostats.add<JsonObject>();
      row["id"] = thermostat.id;
      row["name"] = thermostat.name;
      row["enabled"] = thermostat.enabled;
      row["running"] = thermostat.running;
      row["mode"] = thermostat.mode;
      set_or_null(row, "min_temperature", thermostat.min_temperature);
      set_or_null(row, "max_temperature", thermostat.max_temperature);
      set_or_null(row, "step", thermostat.step);
      row["scheduled_by"] = thermostat.scheduled_by;
      JsonArray presets = row["presets"].to<JsonArray>();
      for (const schedules::PresetTarget &preset : thermostat.presets) {
        JsonObject item = presets.add<JsonObject>();
        item["key"] = preset.key;
        item["name"] = preset.name;
        set_or_null(item, "setpoint", preset.setpoint);
        item["mode"] = preset.mode;
      }
    }
    return true;
  });
  this->answer_(request, ran, 200, "", doc);
}

// Build-time data only, so it stays on the server task.
void WebScheduleEditor::handle_schema_(AsyncWebServerRequest *request) {
  JsonDocument doc;
  doc["version"] = schedules::SCHEDULE_VERSION;
  doc["max_schedules"] = this->engine_->max_schedules();
  doc["max_points_per_day"] = schedules::MAX_POINTS_PER_DAY;
  doc["name_max_length"] = schedules::NAME_MAX_LENGTH;
  doc["max_modes"] = schedules::MAX_MODES;
  doc["max_actions"] = schedules::MAX_ACTIONS;
  doc["mode_name_max_length"] = schedules::MODE_NAME_MAX_LENGTH;
  doc["mode_id_max_length"] = schedules::MODE_ID_MAX_LENGTH;
  doc["min_wait_seconds"] = schedules::MIN_WAIT_SECONDS;
  doc["max_wait_seconds"] = schedules::MAX_WAIT_SECONDS;
  JsonArray days = doc["days"].to<JsonArray>();
  for (const char *day : schedules::DAY_NAMES)
    days.add(day);
  JsonArray colors = doc["colors"].to<JsonArray>();
  for (const char *color : schedules::COLOR_KEYS)
    colors.add(color);
  JsonObject values = doc["values"].to<JsonObject>();
  JsonArray relay = values["relay"].to<JsonArray>();
  for (auto type : {schedules::ValueType::ON, schedules::ValueType::OFF})
    relay.add(schedules::value_type_to_string(type));
  JsonArray thermostat = values["thermostat"].to<JsonArray>();
  for (auto type : {schedules::ValueType::ON, schedules::ValueType::OFF, schedules::ValueType::PRESET,
                    schedules::ValueType::TARGET})
    thermostat.add(schedules::value_type_to_string(type));
  std::string json;
  serializeJson(doc, json);
  request->send(200, "application/json", json.c_str());
}

// --- Writes: the checks and the change go over together, so the answer is what happened ---

// save and import: a document in the body, and the answer says what the engine made of it.
void WebScheduleEditor::handle_document_(AsyncWebServerRequest *request, bool importing) {
  if (this->body_too_large_) {
    this->send_error_(request, "Request body over 16 KiB", 413);
    return;
  }
  if (this->body_no_memory_) {
    ESP_LOGW(TAG, "No room for a %u-byte body", static_cast<unsigned>(this->body_total_));
    this->send_error_(request, schedules::NO_MEMORY_TO_SAVE, 507);
    return;
  }
  if (this->body_ == nullptr) {
    this->send_error_(request, "Empty request body");
    return;
  }
  // A failed engine runs a job in place, on this task's small stack, only to refuse the write:
  // answer before parsing. The engine fails only in its setup, before the server starts.
  if (this->engine_->is_failed()) {
    this->send_error_(request, STORAGE_UNAVAILABLE, 500);
    return;
  }
  JsonDocument answer;
  std::string error;
  int code = 400;
  // Parsed on the loop task too: its stack has the room, the server task's has little.
  const bool ran = this->engine_->run_on_loop([&]() {
    // Before the parse: a small allocation inside it that fails aborts rather than refuses.
    if (!this->engine_->has_heap_room(schedules::PARSE_HEAP_FACTOR * this->body_received_)) {
      code = 507;
      error = schedules::NO_MEMORY_TO_SAVE;
      return true;
    }
    ScheduleConfig config;
    optional<uint32_t> revision;
    {
      JsonDocument doc(this->json_allocator_());
      const DeserializationError parsed =
          deserializeJson(doc, static_cast<const char *>(this->body_), this->body_received_);
      if (parsed == DeserializationError::NoMemory) {
        code = 507;
        error = schedules::NO_MEMORY_TO_SAVE;
        return true;
      }
      if (parsed) {
        error = std::string("JSON parse error: ") + parsed.c_str();
        return true;
      }
      // An import names the schedule it brings back, so its id is required.
      if (!config.deserialize(doc.as<JsonObjectConst>(), importing, &error))
        return true;
      // The revision the form was read at, when it sends one: another client's change since
      // would be undone.
      if (doc["revision"].is<uint32_t>())
        revision = doc["revision"].as<uint32_t>();
    }
    // The engine encodes the schedule again; the body is not needed for that.
    this->reset_body_();
    // A save's id picks the schedule to replace, none or "" creates one; an import's
    // replaces one or creates it under that id.
    const std::string id = config.id;
    const bool replaces = !id.empty() && this->engine_->get(id) != nullptr;
    const Result result = importing    ? this->engine_->restore(std::move(config))
                          : id.empty() ? this->engine_->create(std::move(config))
                                       : this->engine_->update(id, std::move(config), revision);
    if (!result.ok) {
      code = result.code;
      error = result.error;
      return true;
    }
    std::string message = !replaces ? "Schedule created" : importing ? "Schedule replaced" : "Schedule updated";
    message += started_note(*this->engine_, result);
    if (!result.warning.empty())
      message += "; " + result.warning;
    answer["success"] = true;
    answer["message"] = message;
    answer["id"] = result.id;
    // Saved enabled with an action the device skips: a client should not have to parse the message for that.
    if (!result.warning.empty())
      answer["warning"] = result.warning;
    add_ids(answer.as<JsonObject>(), "started", result.started);
    return true;
  });
  if (!error.empty() && code == 507)
    ESP_LOGW(TAG, "%s refused: %s", importing ? "Import" : "Save", error.c_str());
  this->answer_(request, ran, code, error, answer);
}

void WebScheduleEditor::handle_delete_(AsyncWebServerRequest *request) {
  std::string id;
  if (!this->read_id_(request, id))
    return;
  JsonDocument answer;
  std::string error;
  int code = 400;
  const bool ran = this->engine_->run_on_loop([&]() {
    const Result result = this->engine_->remove(id);
    if (!result.ok) {
      code = result.code;
      error = result.error;
      return true;
    }
    answer["success"] = true;
    // The engine blanks a file it cannot unlink; this is the rarer case where that failed too.
    answer["message"] = std::string(result.persisted ? "Schedule deleted"
                                                     : "Schedule deleted; its file could not be removed, so it "
                                                       "comes back at the next boot") +
                        started_note(*this->engine_, result);
    // As on /enable, so a client need not parse the message for it.
    answer["persisted"] = result.persisted;
    add_ids(answer.as<JsonObject>(), "started", result.started);
    return true;
  });
  this->answer_(request, ran, code, error, answer);
}

void WebScheduleEditor::handle_enable_(AsyncWebServerRequest *request) {
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
  JsonDocument answer;
  std::string error;
  int code = 400;
  const bool ran = this->engine_->run_on_loop([&]() {
    const Result result = this->engine_->set_enabled(id, enabled, take_over);
    if (!result.ok) {
      code = result.code;
      error = result.error;
      return true;
    }
    std::string message = enabled ? "Schedule enabled" : "Schedule disabled";
    // Who a take-over stored disabled, then who started on a target a disable let go.
    if (!result.stopped.empty())
      message += "; " + names_of(*this->engine_, result.stopped) + " stopped";
    message += started_note(*this->engine_, result);
    if (!result.warning.empty())
      message += "; " + result.warning;
    answer["success"] = true;
    answer["message"] = message;
    answer["persisted"] = result.persisted;
    // Stored enabled with an action the device skips, as a Save answers it.
    if (!result.warning.empty())
      answer["warning"] = result.warning;
    add_ids(answer.as<JsonObject>(), "stopped", result.stopped);
    add_ids(answer.as<JsonObject>(), "started", result.started);
    return true;
  });
  this->answer_(request, ran, code, error, answer);
}

// --- Answers ---

void WebScheduleEditor::answer_(AsyncWebServerRequest *request, bool ran, int code, const std::string &error,
                                const JsonDocument &doc) {
  if (!ran) {
    // Nothing was read or written, so the same call can simply be made again.
    this->send_error_(request, "Device busy", 503);
  } else if (!error.empty()) {
    this->send_error_(request, error, code);
  } else {
    this->send_doc_(request, doc);
  }
}

void WebScheduleEditor::send_doc_(AsyncWebServerRequest *request, const JsonDocument &doc) {
  // A failed allocation drops members silently; what is left would still serialize.
  if (doc.overflowed()) {
    ESP_LOGW(TAG, "An answer did not fit the heap");
    this->send_error_(request, LOW_MEMORY, 503);
    return;
  }
  ChunkWriter writer(request, this->out_, sizeof(this->out_));
  serializeJson(doc, writer);
  writer.finish();
}

void WebScheduleEditor::send_json_(AsyncWebServerRequest *request, int code, const std::string &json) {
  const char *status = status_line(code);
  if (status == nullptr) {
    request->send(code, "application/json", json.c_str());
  } else {
    this->send_status_(request, status, nullptr, json.c_str());
  }
}

void WebScheduleEditor::send_error_(AsyncWebServerRequest *request, const std::string &message, int code) {
  JsonDocument doc;
  doc["success"] = false;
  doc["error"] = message;
  std::string json;
  serializeJson(doc, json);
  this->send_json_(request, code, json);
}

void WebScheduleEditor::send_status_(AsyncWebServerRequest *request, const char *status, const char *allow,
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

}  // namespace esphome::web_schedule_editor
