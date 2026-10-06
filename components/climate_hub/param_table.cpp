#include "param_table.h"
#include <cmath>
#include <cstring>

namespace esphome::climate_hub {

// Ranges follow ESPHome's own pid and bang_bang platforms where they have an equivalent; the
// rest are what a relay-driven heater can survive.
const ParamDesc PARAMS[] = {
    // control
    {"update_interval_s", "Update interval", "s", "control", "", 30.f, 1.f, 3600.f, 1.f, true,
     "How often the control law re-reads the sensor and recomputes its output."},

    // visual: the range the target moves in, shown beside the setpoint.
    {"visual_min_temperature", "Minimum temperature", "°C", "visual", "", 5.f, -50.f, 100.f, 0.5f, false,
     "Lowest target a client may set. Also the floor every setpoint is clamped to."},
    {"visual_max_temperature", "Maximum temperature", "°C", "visual", "", 45.f, -50.f, 100.f, 0.5f, false,
     "Highest target a client may set."},
    {"visual_step", "Temperature step", "°C", "visual", "", 0.5f, 0.1f, 5.f, 0.1f, false,
     "Increment a slider or a +/- button moves the target by."},

    // output, for each configured direction. Bang-bang drives the relay at a duty of exactly 0
    // or 1, so the slow-PWM period is a PID knob only; the dwell limits protect the relay in both.
    {"period_s", "PWM period", "s", "output", "pid", 300.f, 1.f, 3600.f, 1.f, true,
     "One full on+off cycle of the slow PWM. Longer is gentler on the relay, slower to respond."},
    {"min_on_s", "Minimum on time", "s", "output", "", 10.f, 0.f, 3600.f, 1.f, true,
     "Once closed, the relay stays closed at least this long — protects a compressor or a boiler from "
     "short-cycling."},
    {"min_off_s", "Minimum off time", "s", "output", "", 10.f, 0.f, 3600.f, 1.f, true,
     "Once opened, the relay stays open at least this long."},

    // safety
    {"sensor_timeout_s", "Sensor timeout", "s", "safety", "", 300.f, 10.f, 86400.f, 1.f, true,
     "No reading for this long and the controller faults and opens every relay."},
    {"safety_max_temperature", "Cut-out temperature", "°C", "safety", "", 60.f, -50.f, 200.f, 0.5f, false,
     "Reading above this and the controller cuts out until it falls back."},

    // pid
    {"kp", "Proportional gain", "", "pid", "pid", 0.6f, 0.f, 1000.f, 0.001f, false,
     "Output per degree of error. Raise it for a faster response, lower it if the temperature oscillates."},
    // A slow floor tunes to an integral gain of a few millionths and a derivative gain in the
    // thousands: a period of hours.
    {"ki", "Integral gain", "", "pid", "pid", 0.0025f, 0.f, 1000.f, 0.000001f, false,
     "How fast the accumulated error closes the last gap. Too high overshoots."},
    {"kd", "Derivative gain", "", "pid", "pid", 0.f, 0.f, 10000.f, 0.001f, false,
     "Reacts to how fast the temperature is moving. Usually left at zero for a slow room."},
    {"min_integral", "Minimum integral", "", "pid", "pid", -1.f, -100.f, 100.f, 0.01f, false,
     "Floor for the accumulated term; keeps it from winding up while the heater cannot keep up."},
    {"max_integral", "Maximum integral", "", "pid", "pid", 1.f, -100.f, 100.f, 0.01f, false,
     "Ceiling for the accumulated term."},
    {"starting_integral_term", "Starting integral", "", "pid", "pid", 0.f, -100.f, 100.f, 0.01f, false,
     "Value the accumulated term starts from after a boot or a restart."},
    {"output_samples", "Output averaging", "samples", "pid", "pid", 1.f, 1.f, 100.f, 1.f, true,
     "Number of outputs averaged before the relay sees them. Smooths a noisy sensor."},
    {"derivative_samples", "Derivative averaging", "samples", "pid", "pid", 8.f, 1.f, 100.f, 1.f, true,
     "Number of samples the rate of change is measured over."},
    {"deadband_threshold_low", "Deadband low", "°C", "pid", "pid", 0.f, -50.f, 0.f, 0.1f, false,
     "How far below the target the calm band reaches. Zero disables the deadband."},
    {"deadband_threshold_high", "Deadband high", "°C", "pid", "pid", 0.f, 0.f, 50.f, 0.1f, false,
     "How far above the target the calm band reaches."},
    {"deadband_kp_multiplier", "Deadband kp ×", "", "pid", "pid", 0.f, 0.f, 1.f, 0.01f, false,
     "Proportional gain is scaled by this inside the deadband."},
    {"deadband_ki_multiplier", "Deadband ki ×", "", "pid", "pid", 0.f, 0.f, 1.f, 0.01f, false,
     "Integral gain is scaled by this inside the deadband."},
    {"deadband_kd_multiplier", "Deadband kd ×", "", "pid", "pid", 0.f, 0.f, 1.f, 0.01f, false,
     "Derivative gain is scaled by this inside the deadband."},
    {"deadband_output_samples", "Deadband averaging", "samples", "pid", "pid", 1.f, 1.f, 100.f, 1.f, true,
     "Output averaging used while inside the deadband."},

    // bang_bang: the band around the target. Both floors are 0.1, which keeps the two switching
    // points apart without a cross-field rule. The labels name the point, not what happens
    // there: heating and cooling use the two ends the opposite way round.
    {"hysteresis_below", "Deviation below target", "°C", "bang_bang", "bang_bang", 0.5f, 0.1f, 20.f, 0.1f, false,
     "How far under the target the lower switching point sits — where heating starts, and where cooling stops."},
    {"hysteresis_above", "Deviation above target", "°C", "bang_bang", "bang_bang", 0.5f, 0.1f, 20.f, 0.1f, false,
     "How far over the target the upper switching point sits — where cooling starts, and where heating stops."},
};

const size_t PARAM_COUNT = sizeof(PARAMS) / sizeof(PARAMS[0]);

const ParamDesc *find_param(const char *key) {
  for (const ParamDesc &desc : PARAMS) {
    if (std::strcmp(desc.key, key) == 0)
      return &desc;
  }
  return nullptr;
}

float clamp_param(const char *key, float value) {
  const ParamDesc *d = find_param(key);
  if (d == nullptr)
    return value;
  if (std::isnan(value))
    return d->def;
  if (value < d->min)
    return d->min;
  if (value > d->max)
    return d->max;
  return d->integer ? std::round(value) : value;
}

}  // namespace esphome::climate_hub
