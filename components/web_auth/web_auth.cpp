#include "web_auth.h"
#include <cstring>
#include "esphome/core/log.h"

namespace esphome::web_auth {

static const char *const TAG = "web_auth";

WebAuth *global_web_auth = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

WebAuth::WebAuth(web_server_base::WebServerBase *base) : base_(base) { global_web_auth = this; }

// What the caller gets told back; the route turns it into the error message.
enum class Fault { OK, EMPTY, TOO_LONG, NOT_ASCII, RESERVED };

// Both schemes carry the credentials as header text, so anything outside printable ASCII
// cannot survive the round trip. Basic splits the pair on the first colon; Digest sends the
// username as a quoted string, which a client escapes and the server reads back raw, so a
// quote or a backslash would never compare equal again and would lock the device out.
static Fault check(const std::string &value, size_t max_len, const char *reserved) {
  if (value.empty())
    return Fault::EMPTY;
  if (value.size() > max_len)
    return Fault::TOO_LONG;
  for (const char c : value) {
    if (c < 0x20 || c > 0x7E)
      return Fault::NOT_ASCII;
    if (std::strchr(reserved, c) != nullptr)
      return Fault::RESERVED;
  }
  return Fault::OK;
}

void WebAuth::setup() {
  // Room for the longest pair up front, so no later publish can reallocate a buffer the
  // server may be reading through the pointers it was handed.
  for (uint8_t slot = 0; slot < 2; slot++) {
    this->username_[slot].reserve(USERNAME_MAX);
    this->password_[slot].reserve(PASSWORD_MAX);
  }
  this->pref_ = global_preferences->make_preference<StoredCredentials>(this->preference_hash_);
  StoredCredentials stored{};
  // A record of a different size does not load at all, so a build that changed the limits
  // falls back to the compiled pair rather than to half a password.
  if (this->pref_.load(&stored) && stored.username[0] != '\0') {
    stored.username[USERNAME_MAX] = '\0';
    stored.password[PASSWORD_MAX] = '\0';
    this->publish_(stored.username, stored.password);
  } else {
    this->publish_(this->default_username_, this->default_password_);
  }
  if (this->is_default())
    ESP_LOGW(TAG, "The web interface still takes the factory password; change it under Settings");
}

void WebAuth::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Web Auth:\n"
                "  Username: %s\n"
                "  Password: %s",
                this->username().c_str(), this->is_default() ? "factory default" : "set");
}

const char *WebAuth::validate(const std::string &username, const std::string &password) {
  switch (check(username, USERNAME_MAX, ":\"\\")) {
    case Fault::EMPTY:
      return "'username' is required";
    case Fault::TOO_LONG:
      return "'username' is over 32 characters";
    case Fault::NOT_ASCII:
      return "'username' must be printable ASCII";
    case Fault::RESERVED:
      return "'username' cannot contain ':', '\"' or '\\'";
    case Fault::OK:
      break;
  }
  // No reserved characters: a password reaches neither header as text — Basic base64s it and
  // Digest only hashes it.
  switch (check(password, PASSWORD_MAX, "")) {
    case Fault::EMPTY:
      return "'password' is required";
    case Fault::TOO_LONG:
      return "'password' is over 64 characters";
    case Fault::NOT_ASCII:
      return "'password' must be printable ASCII";
    case Fault::RESERVED:
    case Fault::OK:
      break;
  }
  return nullptr;
}

void WebAuth::set_credentials(const std::string &username, const std::string &password) {
  if (const char *error = validate(username, password); error != nullptr) {
    ESP_LOGE(TAG, "Refusing the credentials: %s", error);
    return;
  }

  // Stored before applied: a write that fails leaves the device serving what it served
  // before, rather than credentials the next boot would not know about. save() only queues
  // the record on ESP32 — sync() is what reaches NVS, so it is the call that can fail. It
  // flushes every component's pending write, so an unrelated failure refuses this change
  // too; there is no way to tell them apart, and refusing is the safe direction.
  StoredCredentials stored{};
  std::memcpy(stored.username, username.c_str(), username.size());
  std::memcpy(stored.password, password.c_str(), password.size());
  if (!this->pref_.save(&stored) || !global_preferences->sync()) {
    ESP_LOGE(TAG, "Storing the credentials failed; the old ones stay in force");
    return;
  }

  this->publish_(username, password);
  ESP_LOGI(TAG, "Credentials changed for user '%s'", this->username().c_str());
}

WebAuth::Status WebAuth::status() const {
  const uint8_t slot = this->slot_;
  return {this->username_[slot], this->password_[slot].size(),
          this->username_[slot] == this->default_username_ && this->password_[slot] == this->default_password_};
}

void WebAuth::publish_(const std::string &username, const std::string &password) {
  const uint8_t next = this->slot_ ^ 1u;
  this->username_[next] = username;
  this->password_[next] = password;
  this->slot_ = next;
#ifdef USE_WEBSERVER_AUTH
  // Two setters, so a request landing between them sees one new field and one old and is
  // asked to authenticate again. Upstream keeps the pair as two plain pointers and offers no
  // way to swap them together, so an aligned word store is as far as this goes; what the
  // slots buy is that whichever pointer a request reads still leads to an intact string.
  this->base_->set_auth_password(this->password_[next].c_str());
  this->base_->set_auth_username(this->username_[next].c_str());
#endif
}

}  // namespace esphome::web_auth
