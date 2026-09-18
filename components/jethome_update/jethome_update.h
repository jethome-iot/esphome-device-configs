#pragma once

#include <string>

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#include "esphome/components/http_request/http_request.h"
#include "esphome/components/http_request/ota/ota_http_request.h"
#include "esphome/components/update/update_entity.h"

#ifdef USE_ESP32
#include <freertos/FreeRTOS.h>
#endif

namespace esphome::jethome_update {

// What the check hands back to the main loop; error_str is null when info is good.
struct CheckResult {
  update::UpdateInfo info;
  const LogString *error_str{nullptr};
};

/// An update entity over a JetHome firmware server: it polls that server's device manifest and
/// installs the image of its channel through `ota.http_request`.
class JethomeUpdate final : public update::UpdateEntity, public PollingComponent, public ota::OTAStateListener {
 public:
  void setup() override;
  void update() override;

  void perform(bool force) override;
  void check() override { this->update(); }

  void on_ota_state(ota::OTAState state, float progress, uint8_t error) override;

  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_source_url(const std::string &source_url) { this->source_url_ = source_url; }
  void set_request_parent(http_request::HttpRequestComponent *request_parent) {
    this->request_parent_ = request_parent;
  }
  void set_ota_parent(http_request::OtaHttpRequestComponent *ota_parent) { this->ota_parent_ = ota_parent; }

  /// The release channel, as the select changes it while the device runs.
  void set_channel(const std::string &channel);
  const std::string &channel() const { return this->channel_; }

 protected:
  void run_check_(CheckResult &result);
  void finish_check_(CheckResult *result);

  http_request::HttpRequestComponent *request_parent_{nullptr};
  http_request::OtaHttpRequestComponent *ota_parent_{nullptr};
  std::string source_url_;
  std::string channel_;
  // The channel the running check reads: on ESP32 it runs in a task, and set_channel() writes
  // channel_ from the main loop.
  std::string checking_channel_;

  static void update_task(void *params);
#ifdef USE_ESP32
  TaskHandle_t update_task_handle_{nullptr};
#endif
  uint8_t initial_check_remaining_{0};
};

}  // namespace esphome::jethome_update
