#include "bindings.h"
#include <cstring>
#include "esphome/core/application.h"
#include "esphome/core/log.h"

namespace esphome::bindings {

BindingsManager *global_bindings_manager = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

static const char *const TAG = "bindings";

template<typename T, typename V> static T *find_by_key(const V &entities, uint32_t key) {
  for (auto *entity : entities) {
    if (entity->get_object_id_hash() == key && !entity->is_internal())
      return entity;
  }
  return nullptr;
}

static binary_sensor::BinarySensor *get_input(uint32_t input_key) {
  return find_by_key<binary_sensor::BinarySensor>(App.get_binary_sensors(), input_key);
}

static switch_::Switch *get_output(uint32_t output_key) {
  return find_by_key<switch_::Switch>(App.get_switches(), output_key);
}

const char *binding_mode_to_string(BindingMode mode) {
  switch (mode) {
    case BindingMode::TOGGLE:
      return "toggle";
    case BindingMode::FOLLOW:
      return "follow";
    default:
      return "none";
  }
}

bool parse_binding_mode(const char *str, BindingMode &mode) {
  if (str == nullptr)
    return false;
  if (strcmp(str, "none") == 0 || strlen(str) == 0) {
    mode = BindingMode::NONE;
    return true;
  }
  if (strcmp(str, "toggle") == 0) {
    mode = BindingMode::TOGGLE;
    return true;
  }
  if (strcmp(str, "follow") == 0) {
    mode = BindingMode::FOLLOW;
    return true;
  }
  return false;
}

BindingsManager::BindingsManager() { global_bindings_manager = this; }

void BindingsManager::setup() {
  // The one boot-time drive: every entity is up, restore_mode has had its say, and dispatch
  // stays off until this returns.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (size_t i = 0; i < this->bindings_.size(); i++) {
    if (this->bindings_[i].mode != BindingMode::FOLLOW)
      continue;
    auto *sensor = get_input(this->bindings_[i].input_key);
    if (sensor == nullptr || !sensor->has_state())
      continue;
    this->drive_output_(this->bindings_[i].output_key, sensor->state);
  }
  this->ready_ = true;
}

void BindingsManager::dump_config() {
  ESP_LOGCONFIG(TAG, "Bindings: %u", static_cast<unsigned>(this->bindings_.size()));
  for (const auto &binding : this->bindings_) {
    auto *input = get_input(binding.input_key);
    auto *output = get_output(binding.output_key);
    char input_id[OBJECT_ID_MAX_LEN] = "<missing>";
    char output_id[OBJECT_ID_MAX_LEN] = "<missing>";
    if (input != nullptr)
      input->get_object_id_to(input_id);
    if (output != nullptr)
      output->get_object_id_to(output_id);
    ESP_LOGCONFIG(TAG, "  %s -> %s (%s)", input_id, output_id, binding_mode_to_string(binding.mode));
  }
}

void BindingsManager::set_binding(uint32_t output_key, uint32_t input_key, BindingMode mode) {
  if (mode == BindingMode::NONE || input_key == 0) {
    this->remove_binding(output_key);
    return;
  }

  auto *binding = this->find_binding_(output_key);
  if (binding == nullptr) {
    this->bindings_.push_back(Binding{output_key, input_key, mode});
  } else {
    binding->input_key = input_key;
    binding->mode = mode;
  }

  auto *sensor = get_input(input_key);
  if (sensor == nullptr) {
    // Kept, not dropped: the input may not exist on this build, and rewriting the user's
    // configuration over that would lose it.
    ESP_LOGW(TAG, "Input 0x%08X not found for output 0x%08X", static_cast<unsigned>(input_key),
             static_cast<unsigned>(output_key));
    return;
  }

  this->ensure_listener_(sensor, input_key);
  if (mode == BindingMode::FOLLOW && sensor->has_state())
    this->drive_output_(output_key, sensor->state);
}

bool BindingsManager::remove_binding(uint32_t output_key) {
  for (auto it = this->bindings_.begin(); it != this->bindings_.end(); ++it) {
    if (it->output_key == output_key) {
      this->bindings_.erase(it);
      return true;
    }
  }
  return false;
}

BindingsManager::Binding *BindingsManager::find_binding_(uint32_t output_key) {
  for (auto &binding : this->bindings_) {
    if (binding.output_key == output_key)
      return &binding;
  }
  return nullptr;
}

void BindingsManager::ensure_listener_(binary_sensor::BinarySensor *sensor, uint32_t input_key) {
  for (uint32_t key : this->subscribed_) {
    if (key == input_key)
      return;
  }
  this->subscribed_.push_back(input_key);

  // Full-state, not the plain callback: the plain one may swallow the first edge after boot.
  sensor->add_full_state_callback([this, input_key](optional<bool> previous, optional<bool> current) {
    if (!current.has_value())
      return;
    // No previous value = the boot level, a state and not an edge.
    const bool rising = current.value() && previous.has_value() && !previous.value();
    this->on_input_state_(input_key, current.value(), rising);
  });
}

void BindingsManager::on_input_state_(uint32_t input_key, bool state, bool rising) {
  // Boot levels arrive before setup(); setup() applies them once, in one place.
  if (!this->ready_)
    return;

  // Indexed: driving an output runs user actions synchronously, which may append to the table.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (size_t i = 0; i < this->bindings_.size(); i++) {
    if (this->bindings_[i].input_key != input_key)
      continue;
    const uint32_t output_key = this->bindings_[i].output_key;
    switch (this->bindings_[i].mode) {
      case BindingMode::FOLLOW:
        this->drive_output_(output_key, state);
        break;
      case BindingMode::TOGGLE:
        if (rising)
          this->toggle_output_(output_key);
        break;
      default:
        break;
    }
  }
}

void BindingsManager::drive_output_(uint32_t output_key, bool state) {
  auto *output = get_output(output_key);
  if (output == nullptr) {
    ESP_LOGW(TAG, "Output 0x%08X not found", static_cast<unsigned>(output_key));
    return;
  }
  if (state) {
    output->turn_on();
  } else {
    output->turn_off();
  }
}

void BindingsManager::toggle_output_(uint32_t output_key) {
  auto *output = get_output(output_key);
  if (output == nullptr) {
    ESP_LOGW(TAG, "Output 0x%08X not found", static_cast<unsigned>(output_key));
    return;
  }
  output->toggle();
}

}  // namespace esphome::bindings
