#pragma once

// Host stand-in for upstream's web_server_base: the request, handler and server classes an
// HTTP handler is written against, driven by a test instead of a socket. Only what the
// components under test use, with web_server_idf's shapes and dispatch order.

#include "esphome/core/defines.h"
#include "esphome/core/optional.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace esphome {

enum http_method { HTTP_DELETE = 0, HTTP_GET = 1, HTTP_HEAD = 2, HTTP_POST = 3, HTTP_PUT = 4 };

class AsyncWebParameter {
 public:
  AsyncWebParameter(std::string name, std::string value) : name_(std::move(name)), value_(std::move(value)) {}
  const std::string &name() const { return this->name_; }
  const std::string &value() const { return this->value_; }

 protected:
  std::string name_;
  std::string value_;
};

// A response begun before it is sent, as web_server_idf's: the status and content type are set
// when it is begun, the body only when it is sent.
class AsyncWebServerResponse {
 public:
  AsyncWebServerResponse(int code, std::string content_type, const char *data, size_t size)
      : code(code), content_type(std::move(content_type)), body(data, size) {}
  void addHeader(const char *name, const char *value) { this->headers.emplace_back(name, value); }  // NOLINT

  int code;
  std::string content_type;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

class AsyncWebServerRequest {
 public:
  // @p target is the path with its query string. A form-encoded body becomes parameters, as
  // web_server_idf reads it; any other body reaches the handler through handleBody.
  AsyncWebServerRequest(http_method method, const std::string &target, std::string body = "",
                        std::string content_type = "application/json")
      : method_(method), body_(std::move(body)), content_type_(std::move(content_type)) {
    const size_t query = target.find('?');
    this->path_ = target.substr(0, query);
    if (query != std::string::npos)
      this->add_params_(target.substr(query + 1));
    if (this->is_form())
      this->add_params_(this->body_);
  }

  http_method method() const { return this->method_; }
  std::string url() const { return this->path_; }
  size_t contentLength() const { return this->body_.size(); }                  // NOLINT(readability-identifier-naming)
  bool hasParam(const char *name) { return this->getParam(name) != nullptr; }  // NOLINT
  bool hasParam(const std::string &name) { return this->getParam(name.c_str()) != nullptr; }  // NOLINT
  AsyncWebParameter *getParam(const char *name) {  // NOLINT(readability-identifier-naming)
    for (auto &param : this->params_) {
      if (param.name() == name)
        return &param;
    }
    return nullptr;
  }
  AsyncWebParameter *getParam(const std::string &name) { return this->getParam(name.c_str()); }  // NOLINT
  void send(int code, const char *content_type = nullptr, const char *content = nullptr) {
    this->responses++;
    this->response_code = code;
    this->response_type = content_type == nullptr ? "" : content_type;
    this->response_body = content == nullptr ? "" : content;
  }
  // NOLINTNEXTLINE(readability-identifier-naming)
  AsyncWebServerResponse *beginResponse(int code, const char *content_type, const uint8_t *data, size_t size) {
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return new AsyncWebServerResponse(code, content_type, reinterpret_cast<const char *>(data), size);
  }
  void send(AsyncWebServerResponse *response) {
    this->send(response->code, response->content_type.c_str());
    this->response_body = response->body;
    this->response_headers = std::move(response->headers);
    delete response;  // NOLINT(cppcoreguidelines-owning-memory)
  }

  // Content-Type only: the one header the handlers under test read. HTTP names are
  // case-insensitive, and upstream's ESP-IDF lookup is too.
  optional<std::string> get_header(const char *name) const {
    const std::string wanted(name);
    if (wanted.size() != sizeof("Content-Type") - 1 ||
        !std::equal(wanted.begin(), wanted.end(), "Content-Type", [](char a, char b) {
          return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        }))
      return {};
    if (this->content_type_.empty())
      return {};
    return this->content_type_;
  }

  const std::string &body() const { return this->body_; }
  bool is_form() const { return this->content_type_.find("application/x-www-form-urlencoded") != std::string::npos; }

  // What the handler answered. `responses` catches a handler that answered twice.
  int response_code{0};
  std::string response_type;
  std::string response_body;
  int responses{0};
  std::vector<std::pair<std::string, std::string>> response_headers;

 protected:
  void add_params_(const std::string &query) {
    size_t start = 0;
    while (start <= query.size()) {
      size_t end = query.find('&', start);
      if (end == std::string::npos)
        end = query.size();
      const std::string pair = query.substr(start, end - start);
      const size_t eq = pair.find('=');
      if (!pair.empty())
        this->params_.emplace_back(pair.substr(0, eq), eq == std::string::npos ? "" : pair.substr(eq + 1));
      start = end + 1;
    }
  }

  http_method method_;
  std::string path_;
  std::string body_;
  std::string content_type_;
  std::vector<AsyncWebParameter> params_;
};

class AsyncWebHandler {
 public:
  virtual ~AsyncWebHandler() {}
  virtual bool canHandle(AsyncWebServerRequest *request) const { return false; }                        // NOLINT
  virtual void handleRequest(AsyncWebServerRequest *request) {}                                         // NOLINT
  virtual void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index,  // NOLINT
                            uint8_t *data, size_t len, bool final) {}
  virtual void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,  // NOLINT
                          size_t total) {}
  virtual bool isRequestHandlerTrivial() const { return true; }  // NOLINT(readability-identifier-naming)
};

class AsyncWebServer {
 public:
  explicit AsyncWebServer(uint16_t port) : port_(port) {}
  void begin() {}
  void end() {}
  void addHandler(AsyncWebHandler *handler) { this->handlers_.push_back(handler); }  // NOLINT

  // The path web_server_idf takes: the first handler that claims the request gets a raw body
  // through handleBody in chunks, then handleRequest. False when no handler claimed it.
  bool dispatch(AsyncWebServerRequest &request, size_t chunk = 512) {
    for (auto *handler : this->handlers_) {
      if (!handler->canHandle(&request))
        continue;
      if (!request.is_form()) {
        std::string body = request.body();
        for (size_t index = 0; index < body.size(); index += chunk) {
          const size_t len = std::min(chunk, body.size() - index);
          handler->handleBody(&request, reinterpret_cast<uint8_t *>(&body[index]), len, index, body.size());
        }
      }
      handler->handleRequest(&request);
      return true;
    }
    return false;
  }

 protected:
  uint16_t port_;
  std::vector<AsyncWebHandler *> handlers_;
};

namespace web_server_base {

class WebServerBase {
 public:
  void init() {
    this->initialized_++;
    if (this->server_ != nullptr)
      return;
    this->server_ = new AsyncWebServer(this->port_);  // NOLINT(cppcoreguidelines-owning-memory)
    for (auto *handler : this->handlers_)
      this->server_->addHandler(handler);
  }
  void deinit() {
    if (this->initialized_ > 0)
      this->initialized_--;
  }
  AsyncWebServer *get_server() const { return this->server_; }
#ifdef USE_WEBSERVER_AUTH
  // Upstream keeps the two strings by pointer and never copies them; so does this, so a test
  // reads back exactly what the component handed over. No middleware: upstream's 401 is the
  // ESP-IDF server's, and re-implementing it here would only prove the copy right.
  void set_auth_username(const char *auth_username) { this->auth_username_ = auth_username; }
  void set_auth_password(const char *auth_password) { this->auth_password_ = auth_password; }
  const char *get_auth_username() const { return this->auth_username_; }
  const char *get_auth_password() const { return this->auth_password_; }
#endif
  void add_handler(AsyncWebHandler *handler) {
    this->handlers_.push_back(handler);
    if (this->server_ != nullptr)
      this->server_->addHandler(handler);
  }
  void add_handler_without_auth(AsyncWebHandler *handler) { this->add_handler(handler); }
  void set_port(uint16_t port) { this->port_ = port; }
  uint16_t get_port() const { return this->port_; }

 protected:
  uint8_t initialized_{0};
  uint16_t port_{80};
  AsyncWebServer *server_{nullptr};
  std::vector<AsyncWebHandler *> handlers_;
#ifdef USE_WEBSERVER_AUTH
  const char *auth_username_{nullptr};
  const char *auth_password_{nullptr};
#endif
};

}  // namespace web_server_base
}  // namespace esphome
