#pragma once

#include <cstdint>
#include <string>
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/preferences.h"

namespace esphome::web_auth {

// The username also goes into a quoted Digest header parameter and, under Basic, into the
// "user:pass" buffer web_server_idf builds on the stack; both are far larger than these.
static const size_t USERNAME_MAX = 32;
static const size_t PASSWORD_MAX = 64;

// One flash record. Fixed-width, so the preference never changes size.
struct StoredCredentials {
  char username[USERNAME_MAX + 1];
  char password[PASSWORD_MAX + 1];
};

// The credentials web_server checks, replaceable while it runs. WebServerBase keeps the two
// strings by pointer and never copies them, so they live here — in two slots, because the
// server's task may be reading the pair at the moment a new one is written.
class WebAuth : public Component {
 public:
  explicit WebAuth(web_server_base::WebServerBase *base);

  // Ahead of every handler registration (web_server is WIFI - 1), so the server never
  // answers a request against the compiled defaults once something is stored.
  float get_setup_priority() const override { return setup_priority::HARDWARE + 1.0f; }
  void setup() override;
  void dump_config() override;

  void set_preference_hash(uint32_t hash) { this->preference_hash_ = hash; }
  // Generated string literals: they outlive the component.
  void set_default_credentials(const char *username, const char *password) {
    this->default_username_ = username;
    this->default_password_ = password;
  }

  // Why the pair cannot be used, or nullptr. Pure, so a caller can answer before storing.
  static const char *validate(const std::string &username, const std::string &password);
  // Stores and applies a validated pair. Call from the main loop: the preference backend
  // keeps its pending writes in a list the loop task flushes, with no lock of its own.
  void set_credentials(const std::string &username, const std::string &password);

  // One consistent view, latched from a single read of the live slot: a route answers from
  // the server's task while the loop may be publishing a new pair.
  struct Status {
    std::string username;
    size_t password_length;
    bool is_default;
  };
  Status status() const;

  const std::string &username() const { return this->username_[this->slot_]; }
  size_t password_length() const { return this->password_[this->slot_].size(); }
  // Still what the firmware was built with — the dashboard says so, and so does the boot log.
  bool is_default() const {
    return this->username() == this->default_username_ && this->password_[this->slot_] == this->default_password_;
  }

 protected:
  // Writes the pair into the slot the server is not holding and hands the new pointers over.
  void publish_(const std::string &username, const std::string &password);

  web_server_base::WebServerBase *base_;
  ESPPreferenceObject pref_;
  uint32_t preference_hash_{0};
  const char *default_username_{""};
  const char *default_password_{""};
  // Double-buffered: a publish writes the slot the server is not holding, so the request
  // that may be inside authenticate() keeps reading intact strings. Two are enough while one
  // publish is pending at a time, which is what the route's named defer keeps true.
  std::string username_[2];
  std::string password_[2];
  uint8_t slot_{0};
};

extern WebAuth *global_web_auth;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::web_auth
