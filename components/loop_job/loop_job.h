#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include "esphome/core/application.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace esphome::loop_job {

/// How long a caller waits for the loop task before it answers for itself.
static const uint32_t LOOP_JOB_TIMEOUT_MS = 5000;

/// The handshake between a caller waiting on another task and the loop task running its job.
/// Both sides race for the same slot once the wait is up, and exactly one of run() and
/// abandon() wins it: a job never runs after its caller has been told that it did not.
class LoopJobSlot {
 public:
  explicit LoopJobSlot(std::function<bool()> &&job) : job_(std::move(job)) {}

  /// Loop side: runs the job unless the caller gave up on it first.
  void run() {
    State expected = State::PENDING;
    if (!this->state_.compare_exchange_strong(expected, State::RUNNING))
      return;
    this->result_ = this->job_();
    this->state_.store(State::DONE);
  }

  /// Caller side, once it has waited long enough: true when the job had not started and now
  /// never will. False means it is already running, and the caller waits for the truth.
  bool abandon() {
    State expected = State::PENDING;
    return this->state_.compare_exchange_strong(expected, State::ABANDONED);
  }

  bool done() const { return this->state_.load() == State::DONE; }
  /// What the job returned; only meaningful once done().
  bool result() const { return this->result_; }

 protected:
  enum class State : uint8_t { PENDING, RUNNING, DONE, ABANDONED };

  std::function<bool()> job_;
  std::atomic<State> state_{State::PENDING};
  bool result_{false};
};

/// Runs jobs on the loop task for callers that are not it: an ESP-IDF HTTP handler runs on the
/// server's own task, and the state it edits — entity records, the scheduler, the menu's view
/// of both — belongs to the loop.
class LoopDispatcher {
 public:
  /// From the owner's setup(), which the loop task runs.
  void capture_loop_task() {
#ifdef USE_ESP32
    this->loop_task_ = xTaskGetCurrentTaskHandle();
#endif
  }

  /// Whether the caller is the loop task itself and can simply do the work. True before
  /// capture_loop_task(), and on every platform that has one task.
  bool on_loop_task() const {
#ifdef USE_ESP32
    return this->loop_task_ == nullptr || xTaskGetCurrentTaskHandle() == this->loop_task_;
#else
    return true;
#endif
  }

  /// Hands @p job to the loop task and blocks until it answers. False when the loop never got
  /// to it, which is also what a failed @p owner gives: the scheduler drops its deferred items,
  /// so a caller that can tell asks is_failed() first instead of waiting out the timeout.
  bool run_on_loop(Component *owner, std::function<bool()> &&job) {
    if (this->on_loop_task())
      return job();
#ifdef USE_ESP32
    auto slot = std::make_shared<LoopJobSlot>(std::move(job));
    // What Component::defer() does; it is protected, and the owner is not this class.
    App.scheduler.set_timeout(owner, static_cast<const char *>(nullptr), 0, [slot]() { slot->run(); });
    for (uint32_t waited = 0; !slot->done() && waited < LOOP_JOB_TIMEOUT_MS; waited += 2)
      vTaskDelay(pdMS_TO_TICKS(2));
    if (slot->abandon()) {
      ESP_LOGE("loop_job", "The loop task did not run the request in time");
      return false;
    }
    // It started right at the deadline: it will finish, and the caller gets the truth.
    while (!slot->done())
      vTaskDelay(pdMS_TO_TICKS(2));
    return slot->result();
#else
    return job();
#endif
  }

 protected:
#ifdef USE_ESP32
  TaskHandle_t loop_task_{nullptr};
#endif
};

}  // namespace esphome::loop_job
