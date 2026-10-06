#pragma once

#include <cstdint>
#include <string>
#include "esphome/core/defines.h"

// A config without a switch: section has no switch sources; the name must still exist.
namespace esphome::switch_ {
class Switch;
}

namespace esphome::climate_hub {

/// Moves from elsewhere that make a relay contested, counted until CONTEST_QUIET_MS of quiet.
static constexpr uint32_t CONTEST_MOVES = 5;
/// Quiet after the last put-back that forgets the moves before it.
static constexpr uint32_t CONTEST_QUIET_MS = 10 * 60 * 1000;
/// From the second move on, the least a put-back waits, whatever min_on and min_off say.
static constexpr uint32_t PUT_BACK_FLOOR_MS = 10 * 1000;

/// Where a relay last moved to, and when, on the hub's clock.
struct RelaySwitching {
  bool on{false};
  uint64_t ms{0};
};

/// Exclusive hold on one relay, with a minimum on and off dwell. The only place in the
/// component that switches a relay, so contact wear has a single owner. Its clock is the hub's
/// 64-bit one: a relay left alone for months has had all its dwell.
class RelayClaim {
 public:
  RelayClaim(switch_::Switch *sw, std::string owner_id) : sw_(sw), owner_(std::move(owner_id)) {}

  void set_dwell(uint32_t min_on_ms, uint32_t min_off_ms);

  /// Returns what the relay is left at, which lags `want` while a dwell floor still runs. A
  /// relay something else moved is put back: at once the first time, then only once the dwell
  /// of where it was moved to is over, PUT_BACK_FLOOR_MS at least. One moved where the demand
  /// may go now stays.
  bool request(bool want, uint64_t now_ms);

  /// Opens the relay regardless of the min-on floor, and also when the claim already believes
  /// it open but something else closed it. A relay already open is not touched: nothing moves,
  /// so no dwell starts. `paced`: a relay closed from elsewhere is put back as request() puts
  /// it back; otherwise, for a safety cut-out, no reading yet or a teardown, at once.
  void force_off(uint64_t now_ms, bool paced);

  /// Carries on from a switching an earlier claim on the relay made, dwell and all. What the
  /// relay shows at the first look after it is no move from elsewhere: a boot may have
  /// restored it.
  void resume(const RelaySwitching &last);
  /// The last switching this claim made or resumed; false before there is one.
  bool last_switching(RelaySwitching *out) const;

  /// Something else keeps moving the relay: CONTEST_MOVES moves without CONTEST_QUIET_MS of
  /// quiet after a put-back.
  bool contested(uint64_t now_ms) const;
  /// Moves from elsewhere since the last quiet spell.
  uint32_t moves() const { return this->moves_; }

  bool state() const { return this->state_; }
  const std::string &owner() const { return this->owner_; }
  /// A new holder starts with no moves held against it, and, as after resume(), what its first
  /// look finds is no move.
  void set_owner(const std::string &owner);
  switch_::Switch *relay() const { return this->sw_; }

 protected:
  void apply_(bool on, uint64_t now_ms);
  /// Whether the relay has stayed where it is for its min_on or min_off.
  bool dwell_over_(uint64_t now_ms) const;
  /// The switch's own state; the claim's belief when there is no switch.
  bool relay_state_() const;
  /// The relay is not where the claim left it: kept where `want` may go now, else put back.
  void settle_move_(bool want, uint64_t now_ms, bool paced);
  /// Counts a move from elsewhere and logs when it goes back: on this pass, or in `wait_ms`.
  /// Past CONTEST_MOVES, only a count that is a power of two is logged.
  void count_move_(uint32_t wait_ms);
  /// Nothing is left to put back: the quiet that forgets the moves starts now.
  void settled_(uint64_t now_ms);
  /// CONTEST_QUIET_MS have passed since the last put-back with no move since.
  bool quiet_(uint64_t now_ms) const;
  void forget_moves_();

  switch_::Switch *sw_;
  std::string owner_;
  uint32_t min_on_ms_{0};
  uint32_t min_off_ms_{0};
  uint64_t last_change_ms_{0};
  // When the last move from elsewhere was put back or left nothing to put back, counted or not.
  uint64_t put_back_ms_{0};
  uint32_t moves_{0};
  // A move went back at once since the last quiet spell, counted or not: the next one is paced.
  bool moved_{false};
  bool state_{false};
  bool initialized_{false};
  // state_ is where something else moved the relay, held until its dwell is over.
  bool pending_{false};
  // Nothing has looked at the relay since the claim was made, resumed or handed over.
  bool fresh_{true};
};

}  // namespace esphome::climate_hub
