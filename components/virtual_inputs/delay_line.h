#pragma once

#include <cstdint>

namespace esphome::virtual_inputs {

/// What a virtual input shows, from its source's level and the clock alone: on once the source
/// has stayed on for the on delay, off once the off delay has passed since the source last went
/// off. Milliseconds on a wrapping 32-bit clock, so a delay stays under 2^31 ms.
class DelayLine {
 public:
  void set_delays(uint32_t on_ms, uint32_t off_ms) {
    this->on_ms_ = on_ms;
    this->off_ms_ = off_ms;
  }
  /// Off, with no source level heard and nothing counting.
  void reset();
  /// The source's level at `now`; a level it had already changes nothing. True when the output
  /// changed at once, which only a zero delay does.
  bool feed(bool level, uint32_t now);
  /// The source's level as it is, any wait counted again from `now`: after a new source or new
  /// delays. True when the output changed at once.
  bool restart(bool level, uint32_t now);
  /// The source has no level: the output holds and nothing counts until it has one.
  void forget_level();
  /// Time passing; true when the output changed.
  bool tick(uint32_t now);

  bool output() const { return this->output_; }
  bool has_level() const { return this->has_level_; }
  /// A change of the output is counting down.
  bool pending() const { return this->pending_; }

 protected:
  bool retarget_(uint32_t now);

  uint32_t on_ms_{0};
  uint32_t off_ms_{0};
  // When the pending change started counting.
  uint32_t since_{0};
  bool output_{false};
  bool level_{false};
  bool has_level_{false};
  bool pending_{false};
};

}  // namespace esphome::virtual_inputs
