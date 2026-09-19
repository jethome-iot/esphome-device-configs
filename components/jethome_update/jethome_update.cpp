#include "jethome_update.h"

#include "esphome/core/log.h"
#include "esphome/core/version.h"

#include "esphome/components/network/util.h"

#include "esphome/components/jethome_manifest/jethome_manifest.h"

namespace esphome::jethome_update {

// The check runs in a task only on ESP32s, and vTaskDelete() never returns.
#ifdef USE_ESP32
#define UPDATE_RETURN \
  do { \
    vTaskDelete(nullptr); \
    __builtin_unreachable(); \
  } while (0)
#else
#define UPDATE_RETURN return
#endif

static const char *const TAG = "jethome_update";

static const size_t MAX_READ_SIZE = 256;
// A device manifest is a couple of kilobytes. Anything larger is not one, and allocating it
// would cost the heap the install itself needs.
static const size_t MAX_MANIFEST_SIZE = 16 * 1024;
static constexpr uint32_t INITIAL_CHECK_INTERVAL_ID = 0;
static constexpr uint32_t INITIAL_CHECK_INTERVAL_MS = 10000;
static constexpr uint8_t INITIAL_CHECK_MAX_ATTEMPTS = 6;

void JethomeUpdate::setup() {
  this->ota_parent_->add_state_listener(this);

  // The first check is worth having early, but only once there is a network. Retry until there
  // is one, unless the poll interval comes round sooner than the retries would end anyway.
  if (this->get_update_interval() != SCHEDULER_DONT_RUN &&
      this->get_update_interval() > INITIAL_CHECK_INTERVAL_MS * INITIAL_CHECK_MAX_ATTEMPTS) {
    this->initial_check_remaining_ = INITIAL_CHECK_MAX_ATTEMPTS;
    this->set_interval(INITIAL_CHECK_INTERVAL_ID, INITIAL_CHECK_INTERVAL_MS, [this]() {
      const bool connected = network::is_connected();
      if (--this->initial_check_remaining_ == 0 || connected) {
        this->cancel_interval(INITIAL_CHECK_INTERVAL_ID);
        if (connected) {
          this->update();
        }
      }
    });
  }
}

void JethomeUpdate::on_ota_state(ota::OTAState state, float progress, uint8_t error) {
  if (state == ota::OTAState::OTA_IN_PROGRESS) {
    this->state_ = update::UPDATE_STATE_INSTALLING;
    this->update_info_.has_progress = true;
    this->update_info_.progress = progress;
    this->publish_state();
  } else if (state == ota::OTAState::OTA_ABORT || state == ota::OTAState::OTA_ERROR) {
    this->status_set_error(LOG_STR("Failed to install firmware"));
    // The channel moved while the install ran, so what it carried is not on offer any more.
    if (this->offered_channel_ != this->channel_) {
      this->update_info_ = {};
      this->state_ = update::UPDATE_STATE_UNKNOWN;
      this->publish_state();
      this->update();
      return;
    }
    this->state_ = update::UPDATE_STATE_AVAILABLE;
    this->publish_state();
  }
}

void JethomeUpdate::set_channel(const std::string &channel) {
  if (channel == this->channel_)
    return;
  this->channel_ = channel;
  // Mid-install the entity describes the image being written; nothing here may touch it, and
  // the end of that install picks the change up.
  if (this->state_ == update::UPDATE_STATE_INSTALLING)
    return;

  // What the last check found was the other channel's; installing it now would be the wrong image.
  this->update_info_ = {};
  this->state_ = update::UPDATE_STATE_UNKNOWN;
  this->status_clear_error();
  // Codegen sets the configured channel before any of this runs.
  if (!this->is_ready())
    return;
  this->publish_state();
  this->update();
}

void JethomeUpdate::update() {
  if (this->state_ == update::UPDATE_STATE_INSTALLING) {
    // The OTA download owns the HTTP stack and the memory for as long as it runs.
    ESP_LOGD(TAG, "Install in progress, skipping update check");
    return;
  }
  if (!network::is_connected()) {
    ESP_LOGD(TAG, "Network not connected, skipping update check");
    return;
  }
  this->cancel_interval(INITIAL_CHECK_INTERVAL_ID);
#ifdef USE_ESP32
  if (this->update_task_handle_ != nullptr) {
    ESP_LOGW(TAG, "Update check already in progress");
    return;
  }
#endif
  // The running check reads this, so it is set only while none is running.
  this->checking_channel_ = this->channel_;
#ifdef USE_ESP32
  if (xTaskCreate(JethomeUpdate::update_task, "jethome_update", 8192, (void *) this, 1, &this->update_task_handle_) !=
      pdPASS) {
    ESP_LOGE(TAG, "Failed to start the update check");
    this->update_task_handle_ = nullptr;
    this->status_set_error(LOG_STR("Failed to start update check"));
  }
#else
  JethomeUpdate::update_task(this);
#endif
}

void JethomeUpdate::update_task(void *params) {
  auto *this_update = static_cast<JethomeUpdate *>(params);

  // Everything with a destructor lives in run_check_(): vTaskDelete() below runs none of them.
  auto *result = new CheckResult();
  this_update->run_check_(*result);

  // The entity's state belongs to the main loop, where the API, MQTT and the web server read it.
  this_update->defer([this_update, result]() { this_update->finish_check_(result); });

  UPDATE_RETURN;
}

void JethomeUpdate::run_check_(CheckResult &result) {
  auto container = this->request_parent_->get(this->source_url_);
  if (container == nullptr || container->status_code != http_request::HTTP_STATUS_OK) {
    ESP_LOGE(TAG, "Failed to fetch the manifest from %s", this->source_url_.c_str());
    if (container != nullptr)
      container->end();
    result.error_str = LOG_STR("Failed to fetch manifest");
    return;
  }

  const size_t content_length = container->content_length;
  if (content_length == 0 || content_length > MAX_MANIFEST_SIZE) {
    ESP_LOGE(TAG, "The answer at %s is %zu bytes, which is not a manifest", this->source_url_.c_str(), content_length);
    container->end();
    result.error_str = LOG_STR("Unexpected manifest size");
    return;
  }

  RAMAllocator<uint8_t> allocator;
  uint8_t *data = allocator.allocate(content_length);
  if (data == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %zu bytes for the manifest", content_length);
    container->end();
    result.error_str = LOG_STR("Failed to allocate memory for manifest");
    return;
  }

  const auto read = http_request::http_read_fully(container.get(), data, content_length, MAX_READ_SIZE,
                                                  this->request_parent_->get_timeout());
  const size_t read_size = container->get_bytes_read();
  container->end();
  container.reset();

  if (read.status != http_request::HttpReadStatus::OK) {
    if (read.status == http_request::HttpReadStatus::TIMEOUT) {
      ESP_LOGE(TAG, "Timeout reading the manifest");
    } else {
      ESP_LOGE(TAG, "Error reading the manifest: %d", read.error_code);
    }
    allocator.deallocate(data, content_length);
    result.error_str = LOG_STR("Failed to read manifest");
    return;
  }

  const bool valid =
      jethome_manifest::parse_manifest(data, read_size, this->checking_channel_, this->source_url_, result.info);
  allocator.deallocate(data, content_length);
  if (!valid) {
    result.error_str = LOG_STR("Failed to parse manifest");
    return;
  }

  // The manifest came over TLS; taking its image over plain HTTP would spend a whole download
  // on something no one vouched for.
  if (this->source_url_.compare(0, 8, "https://") == 0 && result.info.firmware_url.compare(0, 7, "http://") == 0) {
    ESP_LOGE(TAG, "The firmware URL %s is not https", result.info.firmware_url.c_str());
    result.info = {};
    result.error_str = LOG_STR("Firmware URL is not https");
    return;
  }

#ifdef ESPHOME_PROJECT_VERSION
  result.info.current_version = ESPHOME_PROJECT_VERSION;
#else
  result.info.current_version = ESPHOME_VERSION;
#endif
}

void JethomeUpdate::finish_check_(CheckResult *result) {
#ifdef USE_ESP32
  this->update_task_handle_ = nullptr;
#endif

  // An install started while this check ran owns the entity now, and it was waiting for the
  // wire this check held.
  if (this->state_ == update::UPDATE_STATE_INSTALLING) {
    delete result;
    if (this->install_pending_) {
      this->install_pending_ = false;
      this->ota_parent_->flash();
    }
    return;
  }

  // The select can move the channel while a check is in flight; that answer is the old
  // channel's, and its image must not be offered under the new one.
  if (this->checking_channel_ != this->channel_) {
    delete result;
    this->update();
    return;
  }

  if (result->error_str != nullptr) {
    this->status_set_error(result->error_str);
    delete result;
    return;
  }

  const bool available =
      !result->info.latest_version.empty() && result->info.latest_version != result->info.current_version;
  const bool announce = available && this->state_ != update::UPDATE_STATE_AVAILABLE;

  this->update_info_ = std::move(result->info);
  this->offered_channel_ = this->checking_channel_;
  this->state_ = available ? update::UPDATE_STATE_AVAILABLE : update::UPDATE_STATE_NO_UPDATE;
  delete result;  // Safe: a moved-from UpdateInfo is still good to destroy

  this->status_clear_error();
  this->publish_state();

  if (announce) {
    this->get_update_available_trigger()->trigger(this->update_info_);
  }
}

void JethomeUpdate::perform(bool force) {
  // `force` reinstalls an image the last check validated; it is not a way past an install that
  // is already running, nor a way to flash an offer the entity does not have.
  if (this->state_ == update::UPDATE_STATE_INSTALLING) {
    ESP_LOGW(TAG, "Install already in progress");
    return;
  }
  if (this->state_ != update::UPDATE_STATE_AVAILABLE && !force) {
    return;
  }
  if (this->update_info_.firmware_url.empty() || this->update_info_.md5.empty()) {
    ESP_LOGW(TAG, "No firmware to install; check for updates first");
    return;
  }

  this->state_ = update::UPDATE_STATE_INSTALLING;
  this->publish_state();

  this->ota_parent_->set_md5(this->update_info.md5);
  this->ota_parent_->set_url(this->update_info.firmware_url);

#ifdef USE_ESP32
  if (this->update_task_handle_ != nullptr) {
    // A check is on the wire; the image download starts when it comes back.
    this->install_pending_ = true;
    return;
  }
#endif
  // Flash in the next loop
  this->defer([this]() { this->ota_parent_->flash(); });
}

}  // namespace esphome::jethome_update
