#pragma once

#include <vector>
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

namespace esphome::bindings {

enum class BindingMode : uint8_t {
  NONE = 0,
  TOGGLE,  // output toggles on the input's rising edge
  FOLLOW,  // output mirrors the input
};

const char *binding_mode_to_string(BindingMode mode);

// False on an unknown name, leaving `mode` untouched.
bool parse_binding_mode(const char *str, BindingMode &mode);

// Drives switches from binary sensors without an automation. One binding per output; events
// dispatch through a table, so rebinding never has to remove a callback. See README.md.
class BindingsManager : public Component {
 public:
  BindingsManager();

  void setup() override;
  void dump_config() override;
  // After every entity's own setup(), so a follow binding has the last word.
  float get_setup_priority() const override { return setup_priority::DATA; }

  // Keys are fnv1 hashes of the object_id. NONE or a zero input clears the binding.
  void set_binding(uint32_t output_key, uint32_t input_key, BindingMode mode);
  bool remove_binding(uint32_t output_key);
  // The next state from this input is a level, not an edge: a flipped inversion re-emits the
  // sensor's state, which follow bindings must see and toggle bindings must not act on.
  void expect_level(uint32_t input_key);

 protected:
  struct Binding {
    uint32_t output_key;
    uint32_t input_key;
    BindingMode mode;
  };

  std::vector<Binding> bindings_;
  // Inputs already carrying a dispatcher: subscribed once each, never removed.
  std::vector<uint32_t> subscribed_;
  // Inputs whose next state was announced with expect_level().
  std::vector<uint32_t> level_only_;
  // Dispatch is dead until setup(): an input's boot level would otherwise drive an output that
  // has not set itself up yet, and be overwritten seconds later anyway.
  bool ready_{false};

  Binding *find_binding_(uint32_t output_key);
  bool take_level_only_(uint32_t input_key);
  void ensure_listener_(binary_sensor::BinarySensor *sensor, uint32_t input_key);
  void on_input_state_(uint32_t input_key, bool state, bool rising);
  void drive_output_(uint32_t output_key, bool state);
  void toggle_output_(uint32_t output_key);
};

extern BindingsManager *global_bindings_manager;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::bindings
