#include "climate_config.h"
#include <cctype>
#include <cmath>
#include "esphome/core/helpers.h"
#include "param_table.h"

namespace esphome::climate_hub {

namespace {

bool fail(std::string *error, const char *message) {
  if (error != nullptr)
    *error = message;
  return false;
}

// as<std::string>() would read a number or an object as its JSON text.
std::string text_of(JsonVariantConst value) { return value.is<const char *>() ? value.as<std::string>() : ""; }

bool is_ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

void serialize_output(JsonObject obj, const OutputConfig &out) {
  obj["relay_id"] = out.relay_id;
  obj["period_s"] = out.period_s;
  obj["min_on_s"] = out.min_on_s;
  obj["min_off_s"] = out.min_off_s;
}

void deserialize_output(const JsonObject &obj, OutputConfig *out) {
  if (obj.isNull())
    return;
  if (!obj["relay_id"].isNull())
    out->relay_id = text_of(obj["relay_id"]);
  out->period_s = obj["period_s"] | out->period_s;
  out->min_on_s = obj["min_on_s"] | out->min_on_s;
  out->min_off_s = obj["min_off_s"] | out->min_off_s;
}

void clamp_output(OutputConfig *out) {
  out->period_s = clamp_param("period_s", out->period_s);
  out->min_on_s = clamp_param("min_on_s", out->min_on_s);
  out->min_off_s = clamp_param("min_off_s", out->min_off_s);
}

static_assert(ENTITY_ID_MAX_LENGTH == 120, "the sentences below name the limit");

// The rules come in two runs because deserialize() checks the mode word between them: the
// order is the editor's contract, which reports the first rule a document breaks.
bool check_wiring(const ClimateConfig &config, std::string *error) {
  if (config.sensor_id.empty())
    return fail(error, "sensor_id is required");
  // Uncapped, an id could grow the file past what the next boot loads.
  if (config.sensor_id.size() > ENTITY_ID_MAX_LENGTH)
    return fail(error, "sensor_id is longer than 120 characters");
  if (!config.heat.configured() && !config.cool.configured())
    return fail(error, "at least one of heat.relay_id / cool.relay_id is required");
  if (config.heat.relay_id.size() > ENTITY_ID_MAX_LENGTH)
    return fail(error, "heat.relay_id is longer than 120 characters");
  if (config.cool.relay_id.size() > ENTITY_ID_MAX_LENGTH)
    return fail(error, "cool.relay_id is longer than 120 characters");
  // Both directions would share one claim, and the second request each tick would overwrite
  // the first: the controller would run silently inert.
  if (config.heat.configured() && config.heat.relay_id == config.cool.relay_id)
    return fail(error, "heat and cool cannot share one relay");
  if (config.visual.max_temperature <= config.visual.min_temperature)
    return fail(error, "visual.max_temperature must be above visual.min_temperature");
  if (config.pid.max_integral < config.pid.min_integral)
    return fail(error, "pid.max_integral must not be below pid.min_integral");
  return true;
}

bool check_mode(const ClimateConfig &config, std::string *error) {
  if (config.mode == HubMode::HEAT && !config.supports_heat())
    return fail(error, "mode 'heat' needs heat.relay_id");
  if (config.mode == HubMode::COOL && !config.supports_cool())
    return fail(error, "mode 'cool' needs cool.relay_id");
  if (config.mode == HubMode::HEAT_COOL && !(config.supports_heat() && config.supports_cool()))
    return fail(error, "mode 'heat_cool' needs both relays");
  if (std::isnan(config.setpoint))
    return fail(error, "setpoint must be a number");
  return true;
}

}  // namespace

std::string slugify_id(const std::string &name) {
  std::string out;
  out.reserve(name.size());
  bool prev_dash = false;
  for (char c : name) {
    auto u = static_cast<unsigned char>(c);
    if (u < 0x80 && std::isalnum(u) != 0) {
      out += static_cast<char>(std::tolower(u));
      prev_dash = false;
    } else if (!prev_dash && !out.empty()) {
      out += '-';
      prev_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  if (out.size() > ID_MAX_LENGTH)
    out.resize(ID_MAX_LENGTH);
  while (!out.empty() && out.back() == '-')
    out.pop_back();
  return out.empty() ? "climate" : out;
}

std::string trim_name(const std::string &name) {
  size_t begin = 0;
  size_t end = name.size();
  while (begin < end && is_ascii_space(name[begin]))
    begin++;
  while (end > begin && is_ascii_space(name[end - 1]))
    end--;
  return name.substr(begin, end - begin);
}

bool validate_name(const std::string &name, std::string *error) {
  if (name.empty())
    return fail(error, "Name is required");
  if (name.size() > NAME_MAX_LENGTH)
    return fail(error, "Name is longer than 48 characters");
  for (char c : name) {
    auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E)
      return fail(error, "Use printable ASCII characters only");
  }
  if (name.find('/') != std::string::npos)
    return fail(error, "Name cannot contain '/'");
  if (name.find('\\') != std::string::npos)
    return fail(error, "Name cannot contain '\\'");
  return true;
}

std::string name_key(const std::string &name) {
  std::string out;
  out.reserve(name.size());
  bool pending_space = false;
  for (char c : name) {
    auto u = static_cast<unsigned char>(c);
    if (is_ascii_space(c)) {
      pending_space = !out.empty();
      continue;
    }
    if (pending_space) {
      out += ' ';
      pending_space = false;
    }
    out += u < 0x80 ? static_cast<char>(std::tolower(u)) : c;
  }
  return out;
}

std::string object_id_of_name(const std::string &name) {
  std::string out;
  out.reserve(name.size());
  for (char c : name)
    out += to_sanitized_char(to_snake_case_char(c));
  return out;
}

void ClimateConfig::serialize(JsonObject root) const {
  root["version"] = this->version;
  root["id"] = this->id;
  root["name"] = this->name;
  root["enabled"] = this->enabled;
  root["kind"] = enums::control_kind_to_string(this->kind);
  root["sensor_id"] = this->sensor_id;
  root["update_interval_s"] = this->update_interval_s;

  serialize_output(root["heat"].to<JsonObject>(), this->heat);
  serialize_output(root["cool"].to<JsonObject>(), this->cool);

  JsonObject visual = root["visual"].to<JsonObject>();
  visual["min_temperature"] = this->visual.min_temperature;
  visual["max_temperature"] = this->visual.max_temperature;
  visual["step"] = this->visual.step;

  JsonObject safety = root["safety"].to<JsonObject>();
  safety["sensor_timeout_s"] = this->safety.sensor_timeout_s;
  safety["max_temperature"] = this->safety.max_temperature;

  JsonObject pid = root["pid"].to<JsonObject>();
  pid["kp"] = this->pid.kp;
  pid["ki"] = this->pid.ki;
  pid["kd"] = this->pid.kd;
  pid["min_integral"] = this->pid.min_integral;
  pid["max_integral"] = this->pid.max_integral;
  pid["starting_integral_term"] = this->pid.starting_integral_term;
  pid["output_samples"] = this->pid.output_samples;
  pid["derivative_samples"] = this->pid.derivative_samples;
  pid["deadband_threshold_low"] = this->pid.deadband_threshold_low;
  pid["deadband_threshold_high"] = this->pid.deadband_threshold_high;
  pid["deadband_kp_multiplier"] = this->pid.deadband_kp_multiplier;
  pid["deadband_ki_multiplier"] = this->pid.deadband_ki_multiplier;
  pid["deadband_kd_multiplier"] = this->pid.deadband_kd_multiplier;
  pid["deadband_output_samples"] = this->pid.deadband_output_samples;

  JsonObject bb = root["bang_bang"].to<JsonObject>();
  bb["below"] = this->bang_bang.below;
  bb["above"] = this->bang_bang.above;

  root["mode"] = enums::mode_to_string(this->mode);
  root["setpoint"] = this->setpoint;
}

EncodeError ClimateConfig::encode(std::string *out, size_t max_bytes, ArduinoJson::Allocator *allocator) const {
  JsonDocument doc(allocator);
  this->serialize(doc.to<JsonObject>());
  // A failed allocation drops members silently; what is left would still serialize.
  if (doc.overflowed())
    return EncodeError::NO_MEMORY;
  if (measureJson(doc) > max_bytes)
    return EncodeError::TOO_LARGE;
  std::string json;
  serializeJson(doc, json);
  *out = std::move(json);
  return EncodeError::NONE;
}

bool ClimateConfig::deserialize(const JsonObject &root, bool require_id, std::string *error) {
  if (root.isNull())
    return fail(error, "document is not an object");

  this->version = root["version"] | this->version;

  if (require_id) {
    if (root["id"].isNull() || root["id"].as<std::string>().empty())
      return fail(error, "id is required");
    this->id = root["id"].as<std::string>();
    // The id becomes a path component: a hand-edited file could otherwise send the next save
    // anywhere on the filesystem.
    if (slugify_id(this->id) != this->id)
      return fail(error, "id must be a slug: lowercase letters, digits and single dashes");
  } else if (!root["id"].isNull()) {
    this->id = text_of(root["id"]);
  }

  if (text_of(root["name"]).empty())
    return fail(error, "name is required");
  // The name rules run last, on the trimmed name: a blank name is refused there.
  this->name = trim_name(root["name"].as<std::string>());
  this->enabled = root["enabled"] | this->enabled;

  if (!root["kind"].isNull() && !enums::control_kind_from_string(root["kind"].as<std::string>(), &this->kind))
    return fail(error, "kind must be 'pid' or 'bang_bang'");

  this->sensor_id = text_of(root["sensor_id"]);
  this->update_interval_s = root["update_interval_s"] | this->update_interval_s;

  deserialize_output(root["heat"].as<JsonObject>(), &this->heat);
  deserialize_output(root["cool"].as<JsonObject>(), &this->cool);

  JsonObject visual = root["visual"].as<JsonObject>();
  if (!visual.isNull()) {
    this->visual.min_temperature = visual["min_temperature"] | this->visual.min_temperature;
    this->visual.max_temperature = visual["max_temperature"] | this->visual.max_temperature;
    this->visual.step = visual["step"] | this->visual.step;
  }

  JsonObject safety = root["safety"].as<JsonObject>();
  if (!safety.isNull()) {
    this->safety.sensor_timeout_s = safety["sensor_timeout_s"] | this->safety.sensor_timeout_s;
    this->safety.max_temperature = safety["max_temperature"] | this->safety.max_temperature;
  }

  JsonObject pid = root["pid"].as<JsonObject>();
  if (!pid.isNull()) {
    this->pid.kp = pid["kp"] | this->pid.kp;
    this->pid.ki = pid["ki"] | this->pid.ki;
    this->pid.kd = pid["kd"] | this->pid.kd;
    this->pid.min_integral = pid["min_integral"] | this->pid.min_integral;
    this->pid.max_integral = pid["max_integral"] | this->pid.max_integral;
    this->pid.starting_integral_term = pid["starting_integral_term"] | this->pid.starting_integral_term;
    this->pid.output_samples = pid["output_samples"] | this->pid.output_samples;
    this->pid.derivative_samples = pid["derivative_samples"] | this->pid.derivative_samples;
    this->pid.deadband_threshold_low = pid["deadband_threshold_low"] | this->pid.deadband_threshold_low;
    this->pid.deadband_threshold_high = pid["deadband_threshold_high"] | this->pid.deadband_threshold_high;
    this->pid.deadband_kp_multiplier = pid["deadband_kp_multiplier"] | this->pid.deadband_kp_multiplier;
    this->pid.deadband_ki_multiplier = pid["deadband_ki_multiplier"] | this->pid.deadband_ki_multiplier;
    this->pid.deadband_kd_multiplier = pid["deadband_kd_multiplier"] | this->pid.deadband_kd_multiplier;
    this->pid.deadband_output_samples = pid["deadband_output_samples"] | this->pid.deadband_output_samples;
  }

  JsonObject bb = root["bang_bang"].as<JsonObject>();
  if (!bb.isNull()) {
    this->bang_bang.below = bb["below"] | this->bang_bang.below;
    this->bang_bang.above = bb["above"] | this->bang_bang.above;
  }
  this->clamp_numbers();

  if (!check_wiring(*this, error))
    return false;
  if (!root["mode"].isNull() && !enums::mode_from_string(root["mode"].as<std::string>(), &this->mode))
    return fail(error, "mode must be one of off/heat/cool/heat_cool");
  this->setpoint = root["setpoint"] | this->setpoint;
  if (!check_mode(*this, error) || !validate_name(this->name, error))
    return false;
  this->clamp_setpoint();
  return true;
}

bool ClimateConfig::validate(std::string *error) const {
  return check_wiring(*this, error) && check_mode(*this, error) && validate_name(this->name, error);
}

void ClimateConfig::clamp_numbers() {
  this->update_interval_s = clamp_param("update_interval_s", this->update_interval_s);
  clamp_output(&this->heat);
  clamp_output(&this->cool);
  this->visual.min_temperature = clamp_param("visual_min_temperature", this->visual.min_temperature);
  this->visual.max_temperature = clamp_param("visual_max_temperature", this->visual.max_temperature);
  this->visual.step = clamp_param("visual_step", this->visual.step);
  this->safety.sensor_timeout_s = clamp_param("sensor_timeout_s", this->safety.sensor_timeout_s);
  this->safety.max_temperature = clamp_param("safety_max_temperature", this->safety.max_temperature);
  PidParams &p = this->pid;
  p.kp = clamp_param("kp", p.kp);
  p.ki = clamp_param("ki", p.ki);
  p.kd = clamp_param("kd", p.kd);
  p.min_integral = clamp_param("min_integral", p.min_integral);
  p.max_integral = clamp_param("max_integral", p.max_integral);
  p.starting_integral_term = clamp_param("starting_integral_term", p.starting_integral_term);
  p.output_samples = clamp_param("output_samples", p.output_samples);
  p.derivative_samples = clamp_param("derivative_samples", p.derivative_samples);
  p.deadband_threshold_low = clamp_param("deadband_threshold_low", p.deadband_threshold_low);
  p.deadband_threshold_high = clamp_param("deadband_threshold_high", p.deadband_threshold_high);
  p.deadband_kp_multiplier = clamp_param("deadband_kp_multiplier", p.deadband_kp_multiplier);
  p.deadband_ki_multiplier = clamp_param("deadband_ki_multiplier", p.deadband_ki_multiplier);
  p.deadband_kd_multiplier = clamp_param("deadband_kd_multiplier", p.deadband_kd_multiplier);
  p.deadband_output_samples = clamp_param("deadband_output_samples", p.deadband_output_samples);
  this->bang_bang.below = clamp_param("hysteresis_below", this->bang_bang.below);
  this->bang_bang.above = clamp_param("hysteresis_above", this->bang_bang.above);
}

void ClimateConfig::clamp_setpoint() {
  if (this->setpoint < this->visual.min_temperature)
    this->setpoint = this->visual.min_temperature;
  if (this->setpoint > this->visual.max_temperature)
    this->setpoint = this->visual.max_temperature;
}

}  // namespace esphome::climate_hub
