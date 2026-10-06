#include "relay_claim.h"
#include "esphome/core/log.h"
#ifdef USE_SWITCH
#include "esphome/components/switch/switch.h"
#endif

namespace esphome::climate_hub {

static const char *const TAG = "climate_hub";

void RelayClaim::set_dwell(uint32_t min_on_ms, uint32_t min_off_ms) {
  this->min_on_ms_ = min_on_ms;
  this->min_off_ms_ = min_off_ms;
}

bool RelayClaim::relay_state_() const {
#ifdef USE_SWITCH
  if (this->sw_ != nullptr)
    return this->sw_->state;
#endif
  return this->state_;
}

bool RelayClaim::request(bool want, uint32_t now_ms) {
  // Forgotten on a pass rather than only judged by the clock, which wraps back into the window.
  if (this->moves_ != 0 && this->quiet_(now_ms))
    this->moves_ = 0;
  if (!this->initialized_) {
    // The first request always lands: there is no dwell to honour before we owned it.
    this->apply_(want, now_ms);
  } else if (this->relay_state_() != this->state_) {
    // A relay moved from elsewhere is settled first, against the switch rather than our belief.
    this->settle_move_(want, now_ms, true);
  } else if (want != this->state_) {
    if (this->dwell_over_(now_ms))
      this->apply_(want, now_ms);
  } else {
    this->settled_(now_ms);
  }
  this->fresh_ = false;
  return this->state_;
}

void RelayClaim::settle_move_(bool want, uint32_t now_ms, bool paced) {
  const bool moved_to = !this->state_;
  // Where the demand may go now anyway, it stays.
  if (want == moved_to && this->dwell_over_(now_ms)) {
    this->apply_(want, now_ms);
    return;
  }
  if (paced && this->moves_ != 0) {
    // From the second move on, where it was moved counts as a switching: it goes back once that
    // position's dwell is over, so a writer that keeps at it gets one switch per dwell, not one
    // per pass. Where the demand goes, it stays.
    this->state_ = moved_to;
    this->last_change_ms_ = now_ms;
    if (want == moved_to) {
      this->settled_(now_ms);
      return;
    }
    this->pending_ = true;
    const uint32_t dwell = moved_to ? this->min_on_ms_ : this->min_off_ms_;
    this->count_move_(dwell > PUT_BACK_FLOOR_MS ? dwell : PUT_BACK_FLOOR_MS);
    return;
  }
  // The first move, or one a cut-out does not wait on: back on this pass, and its dwell restarts
  // there, since it did move. A boot that restored the relay is no move from elsewhere, nor is
  // one where the demand goes, undone only for the claim's own dwell.
  if (!this->fresh_ && want != moved_to)
    this->count_move_(0);
  this->apply_(this->state_, now_ms);
  this->put_back_ms_ = now_ms;
}

void RelayClaim::count_move_(uint32_t wait_ms) {
  this->moves_++;
  // Put back on every pass, as under a cut-out, a writer would flood the log: past the contest
  // only each doubling is logged.
  if (this->moves_ > CONTEST_MOVES && (this->moves_ & (this->moves_ - 1)) != 0)
    return;
#ifdef USE_SWITCH
  if (wait_ms == 0) {
    ESP_LOGI(TAG, "'%s': relay '%s' moved from elsewhere (%u), put back", this->owner_.c_str(),
             this->sw_->get_name().c_str(), static_cast<unsigned>(this->moves_));
  } else {
    ESP_LOGI(TAG, "'%s': relay '%s' moved from elsewhere (%u), put back in %u s", this->owner_.c_str(),
             this->sw_->get_name().c_str(), static_cast<unsigned>(this->moves_), static_cast<unsigned>(wait_ms / 1000));
  }
#endif
}

void RelayClaim::settled_(uint32_t now_ms) {
  if (!this->pending_)
    return;
  this->pending_ = false;
  this->put_back_ms_ = now_ms;
}

bool RelayClaim::quiet_(uint32_t now_ms) const {
  return !this->pending_ && now_ms - this->put_back_ms_ >= CONTEST_QUIET_MS;
}

bool RelayClaim::contested(uint32_t now_ms) const { return this->moves_ >= CONTEST_MOVES && !this->quiet_(now_ms); }

bool RelayClaim::dwell_over_(uint32_t now_ms) const {
  uint32_t dwell = this->state_ ? this->min_on_ms_ : this->min_off_ms_;
  if (this->pending_ && dwell < PUT_BACK_FLOOR_MS)
    dwell = PUT_BACK_FLOOR_MS;
  return now_ms - this->last_change_ms_ >= dwell;
}

void RelayClaim::force_off(uint32_t now_ms, bool paced) {
  if (this->moves_ != 0 && this->quiet_(now_ms))
    this->moves_ = 0;
  if (!this->relay_state_()) {
    // Already open, and known to be: nothing moves, so no dwell starts, not even on a fresh
    // claim. One the claim believes closed is kept open, whoever opened it.
    if (this->state_) {
      this->apply_(false, now_ms);
    } else {
      this->settled_(now_ms);
    }
  } else if (!this->state_) {
    // Closed from elsewhere while the claim believes it open: only the switch can tell.
    this->settle_move_(false, now_ms, paced);
  } else if (!paced || !this->pending_ || this->dwell_over_(now_ms)) {
    // Closed by the claim, it opens now whatever min_on says; closed from elsewhere, it waits
    // like any put-back unless a cut-out cannot.
    this->apply_(false, now_ms);
  }
  this->fresh_ = false;
}

void RelayClaim::resume(const RelaySwitching &last) {
  this->state_ = last.on;
  this->last_change_ms_ = last.ms;
  this->initialized_ = true;
  this->moves_ = 0;
  this->pending_ = false;
  this->fresh_ = true;
}

void RelayClaim::set_owner(const std::string &owner) {
  this->owner_ = owner;
  this->moves_ = 0;
  this->pending_ = false;
}

bool RelayClaim::last_switching(RelaySwitching *out) const {
  if (!this->initialized_)
    return false;
  out->on = this->state_;
  out->ms = this->last_change_ms_;
  return true;
}

void RelayClaim::apply_(bool on, uint32_t now_ms) {
  this->settled_(now_ms);
  this->state_ = on;
  this->last_change_ms_ = now_ms;
  this->initialized_ = true;
#ifdef USE_SWITCH
  if (this->sw_ == nullptr)
    return;
  if (on) {
    this->sw_->turn_on();
  } else {
    this->sw_->turn_off();
  }
#endif
}

}  // namespace esphome::climate_hub
