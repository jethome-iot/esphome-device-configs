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
// cannot survive the round trip. Basic splits the pair on the first colon and Digest quotes
// the username, which rules those two characters out of a username.
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
  switch (check(username, USERNAME_MAX, ":\"")) {
    case Fault::EMPTY:
      return "'username' is required";
    case Fault::TOO_LONG:
      return "'username' is over 32 characters";
    case Fault::NOT_ASCII:
      return "'username' must be printable ASCII";
    case Fault::RESERVED:
      return "'username' cannot contain ':' or '\"'";
    case Fault::OK:
      break;
  }
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
  // before, rather than credentials the next boot would not know about.
  StoredCredentials stored{};
  std::memcpy(stored.username, username.c_str(), username.size());
  std::memcpy(stored.password, password.c_str(), password.size());
  if (!this->pref_.save(&stored)) {
    ESP_LOGE(TAG, "Storing the credentials failed; the old ones stay in force");
    return;
  }
  global_preferences->sync();

  this->publish_(username, password);
  ESP_LOGI(TAG, "Credentials changed for user '%s'", this->username().c_str());
}

void WebAuth::publish_(const std::string &username, const std::string &password) {
  const uint8_t next = this->slot_ ^ 1u;
  this->username_[next] = username;
  this->password_[next] = password;
  this->slot_ = next;
#ifdef USE_WEBSERVER_AUTH
  // Two setters, so a request landing between them sees one new field and one old and is
  // asked to authenticate again; upstream offers no way to swap the pair at once.
  this->base_->set_auth_password(this->password_[next].c_str());
  this->base_->set_auth_username(this->username_[next].c_str());
#endif
}

}  // namespace esphome::web_auth
