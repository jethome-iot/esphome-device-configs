#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/optional.h"

#ifdef USE_ESP32
#include <esp_http_server.h>
#endif

namespace esphome::web_origin_guard {

/// Whether @p request may be served. The rule is web_server's own
/// (WebServer::is_request_origin_allowed_): no Origin header passes, an Origin whose authority
/// is the Host the request was sent to passes, everything else is cross-origin. There is no
/// allow list -- one rule for every handler on the port.
inline bool allowed(AsyncWebServerRequest *request) {
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
  const optional<std::string> host = request->get_header("Host");
  if (!host.has_value() || host->empty())
    return false;
  return origin->compare(scheme_sep + 3, std::string::npos, *host) == 0;
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
