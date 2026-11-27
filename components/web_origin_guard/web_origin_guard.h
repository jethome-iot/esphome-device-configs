#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/application.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/optional.h"

#ifdef USE_CAPTIVE_PORTAL
#include "esphome/components/captive_portal/captive_portal.h"
#include "esphome/components/wifi/wifi_component.h"
#endif

#ifdef USE_ESP32
#include <esp_http_server.h>
#include <lwip/sockets.h>
#endif

namespace esphome::web_origin_guard {

/// Whether a request's Host header names this device, whose node name is @p name: an IP literal
/// (IPv4, or IPv6 in brackets), the name, `<name>.local` or any `<name>.<domain>`, each with or
/// without a port, case aside. DNS rebinding needs a name the attacker controls, which no
/// literal is and no name starting with this device's is unless the attacker knows it.
/// Upstream's web_server checks no Host at all; keep this through an ESPHome bump (#107).
bool host_names_device(std::string_view host, std::string_view name);

/// Whether the captive portal runs: it answers every host then, since a phone looks for it by
/// probing a host of its own vendor's. Its access point too: WiFi turned off (Network mode
/// Ethernet) takes the access point down without ending the portal.
inline bool captive_portal_active() {
#ifdef USE_CAPTIVE_PORTAL
  return captive_portal::global_captive_portal != nullptr && captive_portal::global_captive_portal->is_active() &&
         wifi::global_wifi_component != nullptr && wifi::global_wifi_component->is_ap_active();
#else
  return false;
#endif
}

/// Whether @p request came in on the access point's own address. In Network mode Auto with no
/// WiFi credentials the access point runs beside Ethernet, where a rebound page reaches the
/// device too: the portal's exemption is for the access point alone. False when unsure.
inline bool arrived_on_access_point(AsyncWebServerRequest *request) {
#if defined(USE_ESP32) && defined(USE_CAPTIVE_PORTAL)
  httpd_req_t *req = *request;
  const int fd = httpd_req_to_sockfd(req);
  struct sockaddr_storage local {};
  socklen_t len = sizeof(local);
  if (fd < 0 || getsockname(fd, reinterpret_cast<struct sockaddr *>(&local), &len) != 0)
    return false;
  ip4_addr_t addr{};
  if (local.ss_family == AF_INET) {
    addr.addr = reinterpret_cast<const struct sockaddr_in *>(&local)->sin_addr.s_addr;
#if LWIP_IPV6
  } else if (local.ss_family == AF_INET6) {
    // A dual-stack server socket gives an IPv4 peer's connection as ::ffff:a.b.c.d.
    const uint8_t *bytes = reinterpret_cast<const struct sockaddr_in6 *>(&local)->sin6_addr.s6_addr;
    static const uint8_t V4_MAPPED[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(bytes, V4_MAPPED, sizeof(V4_MAPPED)) != 0)
      return false;
    memcpy(&addr.addr, bytes + 12, sizeof(addr.addr));
#endif
  } else {
    return false;
  }
  return wifi::global_wifi_component != nullptr &&
         network::IPAddress(&addr) == wifi::global_wifi_component->wifi_soft_ap_ip();
#else
  return false;
#endif
}

/// Whether @p request may be served. A Host that names something else than this device is a
/// page that rebound its own name to the device's address, refused whatever its Origin, except
/// a GET while @p portal_active (the portal runs and the request came in on its access point),
/// the one request the portal answers. Then web_server's own rule
/// (WebServer::is_request_origin_allowed_): no Origin header passes, an Origin whose authority
/// is the Host the request was sent to passes, everything else is cross-origin. There is no
/// allow list -- one rule for every handler on the port.
inline bool allowed(AsyncWebServerRequest *request, bool portal_active) {
  const optional<std::string> host = request->get_header("Host");
  const bool portal_request = portal_active && request->method() == HTTP_GET;
  if (!portal_request && host.has_value() && !host->empty()) {
    const StringRef &name = App.get_name();
    if (!host_names_device(*host, std::string_view(name.c_str(), name.size())))
      return false;
  }
  const optional<std::string> origin = request->get_header("Origin");
  // No Origin: not a browser cross-origin request -- curl, a script, the native API client.
  if (!origin.has_value() || origin->empty())
    return true;
  const size_t scheme_sep = origin->find("://");
  // "null" and anything else without an authority belongs to no origin we serve.
  if (scheme_sep == std::string::npos)
    return false;
  // The authority alone against the Host the request was sent to: the device is reached by
  // IP, by mDNS name and by DHCP name, and none of them is known at build time.
  if (!host.has_value() || host->empty())
    return false;
  return origin->compare(scheme_sep + 3, std::string::npos, *host) == 0;
}

inline bool allowed(AsyncWebServerRequest *request) {
  return allowed(request, captive_portal_active() && arrived_on_access_point(request));
}

/// The one answer a refused request gets, from whichever of the two guards saw it.
inline void send_refusal(AsyncWebServerRequest *request) {
#ifdef USE_ESP32
  // By hand because AsyncWebServerRequest::send() turns every status it does not know --
  // 403 among them -- into a 500.
  httpd_req_t *req = *request;
  httpd_resp_set_status(req, "403 Forbidden");
  httpd_resp_set_type(req, "text/plain");
  static const char BODY[] = "Cross-origin request refused";
  httpd_resp_send(req, BODY, sizeof(BODY) - 1);
#else
  request->send(403, "text/plain", "Cross-origin request refused");
#endif
}

// Wraps a web handler so a cross-origin request is refused before the handler sees it. Our own
// components register one of these around themselves, which is what makes it structurally
// impossible to reach them without the check; CrossOriginRefuser below covers every other
// handler on the shared server, ours included, and neither replaces the other.
class WebOriginGuard : public AsyncWebHandler {
 public:
  explicit WebOriginGuard(AsyncWebHandler *next) : next_(next) {}

  // NOLINTNEXTLINE(readability-identifier-naming)
  bool canHandle(AsyncWebServerRequest *request) const override { return this->next_->canHandle(request); }
  // NOLINTNEXTLINE(readability-identifier-naming)
  bool isRequestHandlerTrivial() const override { return this->next_->isRequestHandlerTrivial(); }

  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleRequest(AsyncWebServerRequest *request) override {
    if (!allowed(request)) {
      send_refusal(request);
      return;
    }
    this->next_->handleRequest(request);
  }

  // The body and the upload land before handleRequest, and a handler that streams them writes
  // as they arrive; both stop here so a refused request reaches no file. The answer is
  // handleRequest's, which always follows.
  //
  // The verdict is taken once per request rather than per chunk: a 1 MB write arrives in some
  // seven hundred of them, and each one would otherwise cost two header lookups and the
  // strings they return. The server delivers a body from index 0 and serves one request at a
  // time, which is what every streaming handler here already relies on.
  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {
    if (index == 0)
      this->allowed_ = allowed(request);
    if (!this->allowed_)
      return;
    this->next_->handleBody(request, data, len, index, total);
  }

  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override {
    if (index == 0)
      this->allowed_ = allowed(request);
    if (!this->allowed_)
      return;
    this->next_->handleUpload(request, filename, index, data, len, final);
  }

 protected:
  AsyncWebHandler *next_;
  // The verdict on the request being streamed. False until a first chunk sets it, so a body
  // that somehow arrives without one is dropped rather than passed on unjudged.
  bool allowed_{false};
};

// Claims a request only in order to refuse it: an allowed request is not claimed and falls
// through to the handler that would have served it. Registered ahead of every other handler,
// so it covers what no wrapper reaches -- upstream's REST routes, its OTA handler at /update,
// anything a future component adds to the shared server.
class CrossOriginRefuser : public AsyncWebHandler, public Component {
 public:
  explicit CrossOriginRefuser(web_server_base::WebServerBase *base) : base_(base) {}

  void setup() override {
    this->base_->init();
    // Without auth: a request from another site is refused outright rather than answered with
    // a challenge that makes the browser ask the person for credentials.
    this->base_->add_handler_without_auth(this);
  }

  // Ahead of every component that registers a handler: web_server and ours are WIFI - 1.0 and
  // below, the web_server OTA platform is AFTER_WIFI, and the server serves the first handler
  // that claims a request.
  float get_setup_priority() const override { return setup_priority::WIFI; }

  // NOLINTNEXTLINE(readability-identifier-naming)
  bool canHandle(AsyncWebServerRequest *request) const override { return !allowed(request); }
  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleRequest(AsyncWebServerRequest *request) override { send_refusal(request); }
  // Claimed, so the body and the upload come here: drained into nothing.
  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {}
  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override {}
  // The raw body has to reach handleBody rather than be parsed as form fields.
  // NOLINTNEXTLINE(readability-identifier-naming)
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  web_server_base::WebServerBase *base_;
};

}  // namespace esphome::web_origin_guard
