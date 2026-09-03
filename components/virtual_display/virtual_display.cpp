#include "virtual_display.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/progmem.h"

#include <esp_http_server.h>

#include <cstring>

namespace esphome {
namespace virtual_display {

static const char *const TAG = "virtual_display";

// Front panel page. Kept vanilla on purpose: no build step, no npm, no bundle —
// it has to work in a firmware whose whole point is being cheap to boot.
static const char FRONT_PANEL_HTML[] PROGMEM = R"=====(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Front panel</title>
<style>
  :root { color-scheme: dark; --bg:#14181b; --fg:#d6dde2; --dim:#7c8a94; --accent:#48b95c; }
  body { margin:0; background:var(--bg); color:var(--fg); display:flex; min-height:100vh;
         align-items:center; justify-content:center; flex-direction:column; gap:18px;
         font:14px/1.4 ui-sans-serif,system-ui,sans-serif; }
  canvas { image-rendering:pixelated; border-radius:6px; background:#000;
           box-shadow:0 0 0 10px #23292d, 0 10px 30px #0008; max-width:96vw; }
  .keys { display:flex; flex-wrap:wrap; gap:8px; justify-content:center; max-width:min(560px,96vw); }
  button { background:#23292d; color:var(--fg); border:1px solid #333c42; border-radius:6px;
           padding:9px 15px; font:inherit; cursor:pointer; min-width:64px; }
  button:hover { border-color:var(--accent); }
  button:active { background:var(--accent); color:#0b0e10; }
  .status { color:var(--dim); font-size:12px; }
  .status b { color:var(--fg); font-weight:600; }
</style></head><body>
<canvas id="screen"></canvas>
<div class="keys" id="keys"></div>
<div class="status" id="status">connecting…</div>
<script>
const SCALE = 4, POLL_MS = 120;
// The page answers on both <prefix> and <prefix>/, so build request URLs from
// its own path rather than relying on relative resolution.
const BASE = location.pathname.replace(/\/+$/, '') + '/';
const cv = document.getElementById('screen'), ctx = cv.getContext('2d');
const off = document.createElement('canvas'), octx = off.getContext('2d');
const statusEl = document.getElementById('status');
let W = 0, H = 0, img = null, since = -1, misses = 0;

function paint(buf) {
  const stride = (W + 7) >> 3, px = img.data;
  for (let y = 0; y < H; y++)
    for (let x = 0; x < W; x++) {
      const on = (buf[y * stride + (x >> 3)] >> (7 - (x & 7))) & 1, i = (y * W + x) * 4;
      px[i] = on ? 0xd8 : 0x0a; px[i+1] = on ? 0xe6 : 0x0d; px[i+2] = on ? 0xff : 0x10; px[i+3] = 255;
    }
  octx.putImageData(img, 0, 0);
  ctx.drawImage(off, 0, 0, cv.width, cv.height);
}

async function poll() {
  try {
    const r = await fetch(`${BASE}frame?since=${since}`, {cache: 'no-store'});
    if (!r.ok) throw new Error(r.status);
    since = parseInt(r.headers.get('X-Frame-Id') || '0', 10);
    const buf = r.status === 204 ? null : new Uint8Array(await r.arrayBuffer());
    if (buf && buf.length) paint(buf);   // 204: still the frame we already have
    misses = 0;
    statusEl.innerHTML = `frame <b>${since}</b> · ${W}×${H}`;
  } catch (e) {
    statusEl.textContent = (++misses > 3) ? 'device not answering' : 'reconnecting…';
  }
  setTimeout(poll, POLL_MS);
}

async function key(name, action) {
  const q = action ? `?action=${action}` : '';
  try { await fetch(`${BASE}key/${encodeURIComponent(name)}${q}`, {method: 'POST'}); } catch (e) {}
}

// Arrows/Enter/Escape map onto same-named keys when the device declares them,
// so the panel is usable from the keyboard as well as the mouse.
const KEYMAP = {ArrowUp:'up', ArrowDown:'down', ArrowLeft:'left', ArrowRight:'right',
                Enter:'enter', Escape:'esc', Backspace:'esc'};

async function loadInfo() {
  // Retried: a transient failure here would otherwise leave the page stuck on
  // "connecting…" forever, since poll() is never started.
  for (;;) {
    try {
      const r = await fetch(`${BASE}info`, {cache: 'no-store'});
      if (!r.ok) throw new Error(r.status);
      return await r.json();
    } catch (e) {
      statusEl.textContent = 'device not answering, retrying…';
      await new Promise(done => setTimeout(done, 1000));
    }
  }
}

(async () => {
  const info = await loadInfo();
  W = info.width; H = info.height;
  cv.width = W * SCALE; cv.height = H * SCALE;
  off.width = W; off.height = H;
  img = octx.createImageData(W, H);
  ctx.imageSmoothingEnabled = false;
  const box = document.getElementById('keys');
  const declared = new Set(info.keys);
  for (const name of info.keys) {
    const b = document.createElement('button');
    b.textContent = name;
    b.addEventListener('pointerdown', e => { e.preventDefault(); key(name, 'down'); });
    // pointercancel too: a touch taken over by scrolling or the OS fires neither
    // pointerup nor pointerleave, and the key would stay held.
    for (const ev of ['pointerup', 'pointerleave', 'pointercancel'])
      b.addEventListener(ev, () => key(name, 'up'));
    box.appendChild(b);
  }
  addEventListener('keydown', e => {
    const k = KEYMAP[e.key];
    if (k && declared.has(k) && !e.repeat) { e.preventDefault(); key(k, 'down'); }
  });
  addEventListener('keyup', e => {
    const k = KEYMAP[e.key];
    if (k && declared.has(k)) { e.preventDefault(); key(k, 'up'); }
  });
  poll();
})();
</script></body></html>)=====";

void VirtualDisplay::setup() {
  this->init_internal_(this->get_buffer_length_());
  if (this->buffer_ == nullptr) {
    ESP_LOGE(TAG, "Could not allocate a %ux%u framebuffer", this->width_, this->height_);
    this->mark_failed();
    return;
  }
  this->fill(Color::BLACK);
  this->base_->init();
  this->base_->add_handler(this);
  ESP_LOGCONFIG(TAG, "Virtual display %ux%u served at %s", this->width_, this->height_, this->url_prefix_.c_str());
}

void VirtualDisplay::dump_config() {
  ESP_LOGCONFIG(TAG, "Virtual display:");
  ESP_LOGCONFIG(TAG, "  Resolution: %ux%u", this->width_, this->height_);
  ESP_LOGCONFIG(TAG, "  Front panel: %s", this->url_prefix_.c_str());
#ifdef USE_BINARY_SENSOR
  for (const auto &key : this->keys_)
    ESP_LOGCONFIG(TAG, "  Key '%s' -> %s", key.name.c_str(), key.sensor->get_name().c_str());
#endif
  LOG_UPDATE_INTERVAL(this);
}

void VirtualDisplay::update() {
  this->do_update_();
  this->frame_id_++;
}

void VirtualDisplay::fill(Color color) {
  if (this->buffer_ != nullptr)
    memset(this->buffer_, color.is_on() ? 0xFF : 0x00, this->get_buffer_length_());
}

void HOT VirtualDisplay::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x < 0 || y < 0 || x >= this->width_ || y >= this->height_ || this->buffer_ == nullptr)
    return;
  const size_t pos = static_cast<size_t>(y) * this->stride_() + (static_cast<size_t>(x) >> 3u);
  const uint8_t mask = 1u << (7u - (static_cast<uint8_t>(x) & 7u));
  if (color.is_on()) {
    this->buffer_[pos] |= mask;
  } else {
    this->buffer_[pos] &= ~mask;
  }
}

bool VirtualDisplay::canHandle(AsyncWebServerRequest *request) const {
  // url_to() writes into the caller's buffer and hands back a StringRef over it.
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string url = request->url_to(url_buf);
  return url == this->url_prefix_ || url.rfind(this->url_prefix_ + "/", 0) == 0;
}

void VirtualDisplay::handleRequest(AsyncWebServerRequest *request) {
  char url_buf[AsyncWebServerRequest::URL_BUF_SIZE];
  const std::string url = request->url_to(url_buf);
  const std::string sub = url.size() > this->url_prefix_.size() ? url.substr(this->url_prefix_.size() + 1) : "";

  if (sub.empty()) {
    request->send(request->beginResponse(200, "text/html; charset=utf-8",
                                         reinterpret_cast<const uint8_t *>(FRONT_PANEL_HTML),
                                         sizeof(FRONT_PANEL_HTML) - 1));
    return;
  }
  if (sub == "info") {
    this->handle_info_(request);
    return;
  }
  if (sub == "frame") {
    this->handle_frame_(request);
    return;
  }
  if (sub.rfind("key/", 0) == 0) {
    this->handle_key_(request, sub.substr(4));
    return;
  }
  request->send(404, "text/plain", "Not found");
}

void VirtualDisplay::handle_info_(AsyncWebServerRequest *request) {
  std::string body =
      "{\"width\":" + to_string(this->width_) + ",\"height\":" + to_string(this->height_) + ",\"keys\":[";
#ifdef USE_BINARY_SENSOR
  bool first = true;
  for (const auto &key : this->keys_) {
    if (!first)
      body += ',';
    body += '"' + key.name + '"';
    first = false;
  }
#endif
  body += "]}";
  request->send(request->beginResponse(200, "application/json", body));
}

void VirtualDisplay::handle_frame_(AsyncWebServerRequest *request) {
  if (this->buffer_ == nullptr) {
    // Unreachable: setup() marks the component failed before registering this
    // handler when the allocation fails.
    request->send(404, "text/plain", "No framebuffer");
    return;
  }
  const uint32_t frame_id = this->frame_id_;
  bool unchanged = false;
  if (request->hasParam("since")) {
    auto since = parse_number<uint32_t>(request->arg("since"));
    unchanged = since.has_value() && *since == frame_id;
  }
  AsyncWebServerResponse *response;
  if (unchanged) {
    response = request->beginResponse(204, nullptr);
  } else {
    // Copied out of the framebuffer rather than pointed at it: the response is
    // sent after this returns, and rendering would be writing into it by then.
    const std::string body(reinterpret_cast<const char *>(this->buffer_), this->get_buffer_length_());
    response = request->beginResponse(200, "application/octet-stream", body);
  }
  // httpd stores the pointer and reads it during send, so this has to outlive
  // the call below — no temporary.
  const std::string frame_header = to_string(frame_id);
  response->addHeader("X-Frame-Id", frame_header.c_str());
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

#ifdef USE_BINARY_SENSOR
void VirtualDisplay::press_key_(Key &key) {
  // A release still pending from an earlier press must not land on this one.
  this->cancel_timeout(key.release_id);
  if (key.down)
    return;
  // Publish the release first: `on_press` fires on a false->true edge, so a press
  // onto a sensor something else left held would never re-fire. Free when the key
  // was already up — publish_state() de-dups only after the filter chain.
  key.sensor->publish_state(false);
  key.sensor->publish_state(true);
  key.down = true;
}

void VirtualDisplay::release_key_(Key &key) {
  this->cancel_timeout(key.release_id);
  key.sensor->publish_state(false);
  key.down = false;
}
#endif

void VirtualDisplay::handle_key_(AsyncWebServerRequest *request, const std::string &name) {
#ifdef USE_BINARY_SENSOR
  for (auto &key : this->keys_) {
    if (key.name != name)
      continue;
    if (request->method() != HTTP_POST) {
      // 405 is not one of the codes the IDF layer maps to a status string and
      // would go out as 500, so overwrite the line beginResponse() just set.
      auto *response = request->beginResponse(405, "application/json", "{\"error\":\"use POST\"}");
      httpd_resp_set_status(*request, "405 Method Not Allowed");
      response->addHeader("Allow", "POST");
      request->send(response);
      return;
    }
    const std::string action = request->hasParam("action") ? request->arg("action") : "";
    if (!action.empty() && action != "down" && action != "up") {
      // Falling through to "press" would latch the key on a typo, since only the
      // empty action schedules a release.
      request->send(400, "application/json", "{\"error\":\"action must be down or up\"}");
      return;
    }
    Key *key_ptr = &key;
    const uint32_t hold_time = this->hold_time_;
    // Everything that touches entity state is handed to the main loop: this
    // handler runs on the HTTP server task, while publish_state() walks the
    // filter chain and calls every state callback — automations included —
    // synchronously. The sibling components defer for the same reason.
    //
    // Deliberately unnamed: a named defer replaces the pending callback with the
    // same name, so a fast tap — down and up arriving within one loop tick —
    // would lose the press entirely. Unnamed ones queue in order.
    this->defer([this, key_ptr, action, hold_time]() {
      if (action == "up") {
        this->release_key_(*key_ptr);
        return;
      }
      this->press_key_(*key_ptr);
      if (action.empty()) {
        // Keyed per key, so a second press of the same key restarts its release
        // instead of cancelling an unrelated one.
        this->set_timeout(key_ptr->release_id, hold_time, [this, key_ptr]() { this->release_key_(*key_ptr); });
      }
    });
    ESP_LOGD(TAG, "Key '%s' %s", name.c_str(), action.empty() ? "pressed" : action.c_str());
    request->send(200, "application/json", "{\"ok\":true}");
    return;
  }
#endif
  request->send(404, "application/json", "{\"error\":\"unknown key\"}");
}

}  // namespace virtual_display
}  // namespace esphome
