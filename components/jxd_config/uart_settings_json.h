#pragma once

#include "esphome/core/defines.h"
#ifdef JXD_CONFIG_UART

#include <vector>
#include "esphome/components/config_json/settings_base_json.h"
#include "esphome/components/uart/uart_component.h"
#include "esphome/components/uart_list/uart_list.h"
#include "esphome/core/log.h"

namespace esphome::jxd_config {

struct UartSettingsRecord {
  uint8_t uart_index{0};  // position in uart_list
  uint32_t baud_rate{9600};
  uint8_t parity{0};  // uart::UARTParityOptions: 0 none, 1 even, 2 odd
  uint8_t stop_bits{1};

  uint32_t key() const { return this->uart_index; }

  void to_json(JsonObject obj, uint32_t version) const {
    obj["uart_index"] = this->uart_index;
    obj["baud_rate"] = this->baud_rate;
    obj["parity"] = this->parity;
    obj["stop_bits"] = this->stop_bits;
  }

  bool from_json(JsonObject obj, uint32_t version) {
    if (!obj["uart_index"].is<uint8_t>())
      return false;
    this->uart_index = obj["uart_index"];
    this->baud_rate = obj["baud_rate"] | 9600;
    this->parity = obj["parity"] | 0;
    this->stop_bits = obj["stop_bits"] | 1;
    return true;
  }
};

class UartSettingsJson : public config_json::SettingsBaseJson {
 public:
  static constexpr const char *TAG = "jxd_config.uart";
  static constexpr const char *NAME = "uart";

  ~UartSettingsJson() override { this->clear_records_(); }

  const char *get_key() override { return NAME; }

  void set_uart_list(uart_list::UartList *uart_list) { this->uart_list_ = uart_list; }

  bool parse_json(JsonObject root, uint32_t version) override {
    this->clear_records_();
    if (!root["records"].is<JsonArray>()) {
      ESP_LOGE(TAG, "Invalid 'records' field - must be an array");
      return false;
    }
    JsonArray array = root["records"];
    for (JsonObject obj : array) {
      auto *record = new UartSettingsRecord();  // NOLINT(cppcoreguidelines-owning-memory)
      if (record->from_json(obj, version)) {
        this->records_.push_back(record);
      } else {
        ESP_LOGW(TAG, "Failed to parse uart record");
        delete record;  // NOLINT(cppcoreguidelines-owning-memory)
      }
    }
    ESP_LOGI(TAG, "Loaded %u uart settings", static_cast<unsigned>(this->records_.size()));
    return true;
  }

  void write_json(JsonObject root, uint32_t version) override {
    JsonArray array = root["records"].to<JsonArray>();
    for (const auto *record : this->records_) {
      if (record != nullptr)
        record->to_json(array.add<JsonObject>(), version);
    }
  }

  void apply() override {
    if (this->uart_list_ == nullptr) {
      ESP_LOGW(TAG, "UART list not set, cannot apply settings");
      return;
    }
    for (auto *record : this->records_)
      this->apply_record_(record);
  }

  // Before the UARTs' own setup(); a later apply() reloads the driver with the new values.
  static constexpr float APPLY_PRIORITY = setup_priority::HARDWARE + 1.0f;

  size_t size() override { return this->records_.size(); }

  void reset() override {
    this->clear_records_();
    this->mark_dirty();
    ESP_LOGI(TAG, "Cleared all uart settings");
  }

  UartSettingsRecord *make_record(uint8_t uart_index, uint32_t baud_rate, uint8_t parity, uint8_t stop_bits) {
    UartSettingsRecord *record = nullptr;
    for (auto *rec : this->records_) {
      if (rec != nullptr && rec->uart_index == uart_index) {
        record = rec;
        break;
      }
    }
    if (record == nullptr) {
      record = new UartSettingsRecord();  // NOLINT(cppcoreguidelines-owning-memory)
      record->uart_index = uart_index;
      this->records_.push_back(record);
    }
    record->baud_rate = baud_rate;
    record->parity = parity;
    record->stop_bits = stop_bits;
    this->mark_dirty();
    return record;
  }

  bool get_record(uint8_t uart_index, UartSettingsRecord &record) {
    for (const auto *rec : this->records_) {
      if (rec != nullptr && rec->uart_index == uart_index) {
        record = *rec;
        return true;
      }
    }
    return false;
  }

  std::vector<UartSettingsRecord *> &records() { return this->records_; }

 protected:
  void apply_record_(UartSettingsRecord *record) {
    if (record == nullptr || this->uart_list_ == nullptr)
      return;
    uart::UARTComponent *uart_ptr = this->uart_list_->get(record->uart_index);
    if (uart_ptr == nullptr) {
      ESP_LOGW(TAG, "UART not found at index %u", record->uart_index);
      return;
    }
    uart_ptr->set_baud_rate(record->baud_rate);
    uart_ptr->set_parity(static_cast<uart::UARTParityOptions>(record->parity));
    uart_ptr->set_stop_bits(record->stop_bits);
    uart_ptr->load_settings(false);
    ESP_LOGD(TAG, "Applied settings to uart index %u", record->uart_index);
  }

  void clear_records_() {
    for (auto *record : this->records_)
      delete record;  // NOLINT(cppcoreguidelines-owning-memory)
    this->records_.clear();
  }

  std::vector<UartSettingsRecord *> records_;
  uart_list::UartList *uart_list_{nullptr};
};

}  // namespace esphome::jxd_config

#endif  // JXD_CONFIG_UART
