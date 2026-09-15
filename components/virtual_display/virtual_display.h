#pragma once

#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/components/display/display_buffer.h"
#include "esphome/components/web_server_base/web_server_base.h"

#ifdef USE_BINARY_SENSOR
#include "esphome/components/binary_sensor/binary_sensor.h"
#endif

#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace virtual_display {

/**
 * @brief A display with no bus behind it, served over HTTP instead.
 *
 * Pages, fonts and menus are written against the generic display API, so they
 * render here exactly as they do on the physical panel — the driver is only the
 * thing that pushes the finished buffer somewhere. This one pushes it to a
 * browser, which is what makes a display-carrying device testable under QEMU
 * (no I2C controller is emulated there, so the real SSD1306/SH1106 is dead).
 *
 * Endpoints under `url_prefix`:
 *   GET  /            front panel page (canvas + on-screen keys)
 *   GET  /info        {"width","height","keys":[...]}
 *   GET  /frame       raw 1bpp framebuffer, MSB first, row-major. Served from a
 *                     snapshot taken at the end of a render, so the bytes and
 *                     `X-Frame-Id` always describe the same frame; `?since=<id>`
 *                     answers 204 while that frame is still the newest.
 *   POST /key/<name>  inject a key. `?action=down|up` holds or releases it,
 *                     the default press releases itself after `hold_time`.
 */
class VirtualDisplay : public display::DisplayBuffer, public AsyncWebHandler {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  // Not PROCESSOR like a bus-backed display driver: setup() starts the HTTP
  // server, and doing that before the network stack exists asserts inside lwIP.
  // Matches the other web components; nothing draws before setup anyway.
  float get_setup_priority() const override { return setup_priority::WIFI - 1.0f; }

  void set_base(web_server_base::WebServerBase *base) { this->base_ = base; }
  void set_dimensions(uint16_t width, uint16_t height) {
    this->width_ = width;
    this->height_ = height;
  }
  void set_url_prefix(const std::string &prefix) { this->url_prefix_ = prefix; }
  void set_hold_time(uint32_t hold_time) { this->hold_time_ = hold_time; }

  display::DisplayType get_display_type() override { return display::DisplayType::DISPLAY_TYPE_BINARY; }

  void fill(Color color) override;

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;

#ifdef USE_BINARY_SENSOR
  void add_key(const std::string &name, binary_sensor::BinarySensor *sensor) {
    this->keys_.push_back({name, sensor, false, static_cast<uint32_t>(this->keys_.size())});
  }
#endif

 protected:
  void draw_absolute_pixel_internal(int x, int y, Color color) override;
  int get_width_internal() override { return this->width_; }
  int get_height_internal() override { return this->height_; }
  size_t stride_() const { return (static_cast<size_t>(this->width_) + 7u) / 8u; }
  size_t get_buffer_length_() const { return this->stride_() * static_cast<size_t>(this->height_); }

  void handle_info_(AsyncWebServerRequest *request);
  void handle_frame_(AsyncWebServerRequest *request);
  void handle_key_(AsyncWebServerRequest *request, const std::string &name);

#ifdef USE_BINARY_SENSOR
  struct Key {
    std::string name;
    binary_sensor::BinarySensor *sensor;
    bool down;
    // Scheduler ids live in a per-component namespace, so the key index is enough.
    uint32_t release_id;
  };
  void press_key_(Key &key);
  void release_key_(Key &key);
#endif

  web_server_base::WebServerBase *base_{nullptr};
  std::string url_prefix_{"/panel"};
  uint16_t width_{128};
  uint16_t height_{64};
  uint32_t hold_time_{120};

  // What /frame serves. update() runs on the main loop and the HTTP handler on
  // the httpd task, so the finished frame is published as a snapshot: id and
  // bytes are written and read together under the lock, never torn.
  uint8_t *snapshot_{nullptr};
  // Bumped on every render so the page can poll without re-fetching a frame it
  // already has.
  uint32_t snapshot_id_{0};
  Mutex snapshot_lock_;

#ifdef USE_BINARY_SENSOR
  std::vector<Key> keys_;
#endif
};

}  // namespace virtual_display
}  // namespace esphome
