#include "climate_config.h"
#include <cctype>
#include <cmath>
#include "esphome/core/helpers.h"
#include "param_table.h"

namespace esphome::climate_hub {

namespace {

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
    out->relay_id = obj["relay_id"].as<std::string>();
  out->period_s = clamp_param("period_s", obj["period_s"] | out->period_s);
  out->min_on_s = clamp_param("min_on_s", obj["min_on_s"] | out->min_on_s);
  out->min_off_s = clamp_param("min_off_s", obj["min_off_s"] | out->min_off_s);
}

bool fail(std::string *error, const char *message) {
  if (error != nullptr)
    *error = message;
  return false;
}

bool is_ascii_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

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
    return fail(error, "name is required");
  if (name.size() > NAME_MAX_LENGTH)
    return fail(error, "name must be at most 48 characters");
  for (char c : name) {
    if (c == '/' || c == '\\')
      return fail(error, "name must not contain '/' or '\\'");
    auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || u > 0x7E)
      return fail(error, "name must use printable ASCII characters only");
  }
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
    this->id = root["id"].as<std::string>();
  }

  this->name = root["name"].isNull() ? std::string() : trim_name(root["name"].as<std::string>());
  this->enabled = root["enabled"] | this->enabled;

  if (!root["kind"].isNull() && !enums::control_kind_from_string(root["kind"].as<std::string>(), &this->kind))
    return fail(error, "kind must be 'pid' or 'bang_bang'");

  this->sensor_id = root["sensor_id"].isNull() ? std::string() : root["sensor_id"].as<std::string>();
  this->update_interval_s = clamp_param("update_interval_s", root["update_interval_s"] | this->update_interval_s);

  deserialize_output(root["heat"].as<JsonObject>(), &this->heat);
  deserialize_output(root["cool"].as<JsonObject>(), &this->cool);

  JsonObject visual = root["visual"].as<JsonObject>();
  if (!visual.isNull()) {
    this->visual.min_temperature =
        clamp_param("visual_min_temperature", visual["min_temperature"] | this->visual.min_temperature);
    this->visual.max_temperature =
        clamp_param("visual_max_temperature", visual["max_temperature"] | this->visual.max_temperature);
    this->visual.step = clamp_param("visual_step", visual["step"] | this->visual.step);
  }

  JsonObject safety = root["safety"].as<JsonObject>();
  if (!safety.isNull()) {
    this->safety.sensor_timeout_s =
        clamp_param("sensor_timeout_s", safety["sensor_timeout_s"] | this->safety.sensor_timeout_s);
    this->safety.max_temperature =
        clamp_param("safety_max_temperature", safety["max_temperature"] | this->safety.max_temperature);
  }

  JsonObject pid = root["pid"].as<JsonObject>();
  if (!pid.isNull()) {
    this->pid.kp = clamp_param("kp", pid["kp"] | this->pid.kp);
    this->pid.ki = clamp_param("ki", pid["ki"] | this->pid.ki);
    this->pid.kd = clamp_param("kd", pid["kd"] | this->pid.kd);
    this->pid.min_integral = clamp_param("min_integral", pid["min_integral"] | this->pid.min_integral);
    this->pid.max_integral = clamp_param("max_integral", pid["max_integral"] | this->pid.max_integral);
    this->pid.starting_integral_term =
        clamp_param("starting_integral_term", pid["starting_integral_term"] | this->pid.starting_integral_term);
    this->pid.output_samples = clamp_param("output_samples", pid["output_samples"] | this->pid.output_samples);
    this->pid.derivative_samples =
        clamp_param("derivative_samples", pid["derivative_samples"] | this->pid.derivative_samples);
    this->pid.deadband_threshold_low =
        clamp_param("deadband_threshold_low", pid["deadband_threshold_low"] | this->pid.deadband_threshold_low);
    this->pid.deadband_threshold_high =
        clamp_param("deadband_threshold_high", pid["deadband_threshold_high"] | this->pid.deadband_threshold_high);
    this->pid.deadband_kp_multiplier =
        clamp_param("deadband_kp_multiplier", pid["deadband_kp_multiplier"] | this->pid.deadband_kp_multiplier);
    this->pid.deadband_ki_multiplier =
        clamp_param("deadband_ki_multiplier", pid["deadband_ki_multiplier"] | this->pid.deadband_ki_multiplier);
    this->pid.deadband_kd_multiplier =
        clamp_param("deadband_kd_multiplier", pid["deadband_kd_multiplier"] | this->pid.deadband_kd_multiplier);
    this->pid.deadband_output_samples =
        clamp_param("deadband_output_samples", pid["deadband_output_samples"] | this->pid.deadband_output_samples);
  }

  JsonObject bb = root["bang_bang"].as<JsonObject>();
  if (!bb.isNull()) {
    this->bang_bang.below = clamp_param("hysteresis_below", bb["below"] | this->bang_bang.below);
    this->bang_bang.above = clamp_param("hysteresis_above", bb["above"] | this->bang_bang.above);
  }

  if (!root["mode"].isNull() && !enums::mode_from_string(root["mode"].as<std::string>(), &this->mode))
    return fail(error, "mode must be one of off/heat/cool/heat_cool");

  this->setpoint = root["setpoint"] | this->setpoint;

  if (!this->validate(error))
    return false;
  this->clamp_setpoint();
  return true;
}

bool ClimateConfig::validate(std::string *error) const {
  if (!validate_name(this->name, error))
    return false;
  if (this->sensor_id.empty())
    return fail(error, "sensor_id is required");
  if (!this->heat.configured() && !this->cool.configured())
    return fail(error, "at least one of heat.relay_id / cool.relay_id is required");
  // Both directions would share one claim, and the second request each tick would overwrite
  // the first: the controller would run silently inert.
  if (this->heat.configured() && this->heat.relay_id == this->cool.relay_id)
    return fail(error, "heat and cool cannot share one relay");
  if (this->visual.max_temperature <= this->visual.min_temperature)
    return fail(error, "visual.max_temperature must be above visual.min_temperature");
  if (this->pid.max_integral < this->pid.min_integral)
    return fail(error, "pid.max_integral must not be below pid.min_integral");
  if (this->mode == HubMode::HEAT && !this->supports_heat())
    return fail(error, "mode 'heat' needs heat.relay_id");
  if (this->mode == HubMode::COOL && !this->supports_cool())
    return fail(error, "mode 'cool' needs cool.relay_id");
  if (this->mode == HubMode::HEAT_COOL && !(this->supports_heat() && this->supports_cool()))
    return fail(error, "mode 'heat_cool' needs both relays");
  if (std::isnan(this->setpoint))
    return fail(error, "setpoint must be a number");
  return true;
}

void ClimateConfig::clamp_setpoint() {
  if (this->setpoint < this->visual.min_temperature)
    this->setpoint = this->visual.min_temperature;
  if (this->setpoint > this->visual.max_temperature)
    this->setpoint = this->visual.max_temperature;
}

}  // namespace esphome::climate_hub
