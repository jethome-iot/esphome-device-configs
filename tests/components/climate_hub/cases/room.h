#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace esphome::climate_hub::testing {

// A room a calibration can swing: it drifts towards the air outside with a time constant, and
// its relay adds `drive` °C per hour at full, heating or, negative, cooling. What the relay does
// reaches the probe `dead_s` later, as a radiator or a screed delays it. The probe reads to
// `resolution` (a DS18B20's 0.0625, or 0 for exact), plus a deterministic noise of up to
// ±`noise`.
struct RoomModel {
  float start{18.f};
  float outside{10.f};
  float drive{10.f};
  float tau_h{2.f};
  float dead_s{300.f};
  float step_s{10.f};
  float resolution{0.f};
  float noise{0.f};
};

class Room {
 public:
  explicit Room(const RoomModel &model)
      : model_(model),
        line_(static_cast<size_t>(std::lround(model.dead_s / model.step_s)) + 1, false),
        temperature_(model.start) {}

  // One step of step_s with the relay as it is now; returns what the probe reads after it.
  float step(bool relay_on) {
    this->line_[this->head_] = relay_on;
    this->head_ = (this->head_ + 1) % this->line_.size();
    // The oldest in the line: what the relay did dead_s ago.
    const bool arriving = this->line_[this->head_];
    const float hours = this->model_.step_s / 3600.f;
    this->temperature_ += hours * ((arriving ? this->model_.drive : 0.f) -
                                   (this->temperature_ - this->model_.outside) / this->model_.tau_h);
    return this->reading();
  }

  float reading() {
    float value = this->temperature_;
    if (this->model_.noise > 0.f) {
      // A linear congruential generator: the same noise on every run.
      this->seed_ = this->seed_ * 1664525u + 1013904223u;
      value += this->model_.noise * (static_cast<float>(this->seed_ >> 8) / 8388608.f - 1.f);
    }
    if (this->model_.resolution > 0.f)
      value = std::round(value / this->model_.resolution) * this->model_.resolution;
    return value;
  }

  float temperature() const { return this->temperature_; }

 protected:
  RoomModel model_;
  std::vector<bool> line_;
  size_t head_{0};
  float temperature_;
  uint32_t seed_{12345};
};

// A room with radiators: an hour or two of swinging around the target.
inline RoomModel radiator_room() { return RoomModel{}; }

// A screed floor: slow to warm and to cool, the heat half an hour in coming.
inline RoomModel floor_heating() {
  RoomModel model;
  model.start = 20.f;
  model.drive = 2.f;
  model.tau_h = 12.f;
  model.dead_s = 1800.f;
  model.step_s = 30.f;
  return model;
}

}  // namespace esphome::climate_hub::testing
