#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/host/preferences.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/web_auth/web_auth.h"
#include "esphome/core/preferences.h"

namespace esphome::web_auth::testing {

static const uint32_t PREF_HASH = 0x7EB0A147;

// What the component said. Both lines a user is promised — the boot warning and the
// dump_config row — exist only in the log. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> lines;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() { this->lines.clear(); }
  bool has(const char *needle) const {
    return std::any_of(this->lines.begin(), this->lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t, const char *, const char *message, size_t len) {
    static_cast<LogCapture *>(self)->lines.emplace_back(message, len);
  }
};

class WebAuthTest : public ::testing::Test {
 protected:
  void SetUp() override {
    LogCapture::instance().clear();
    host::setup_preferences();
    // sync() first: it is what runs the backend's lazy read of the file, and a reset before
    // that would be undone by the next call that triggers it.
    global_preferences->sync();
    global_preferences->reset();
    global_preferences->sync();
  }

  // A device coming up: codegen's pair, then whatever was stored. Heap-held, because the
  // server is handed pointers into this object's strings.
  std::unique_ptr<WebAuth> boot(web_server_base::WebServerBase *base, uint32_t hash = PREF_HASH) {
    auto auth = std::make_unique<WebAuth>(base);
    auth->set_default_credentials("admin", "hunter2");
    auth->set_preference_hash(hash);
    auth->setup();
    return auth;
  }
  std::unique_ptr<WebAuth> boot() { return this->boot(&this->base_); }

  web_server_base::WebServerBase base_;
};

TEST_F(WebAuthTest, HandsTheServerTheCompiledPairWhenNothingIsStored) {
  auto auth = this->boot();
  EXPECT_EQ(std::string(this->base_.get_auth_username()), "admin");
  EXPECT_EQ(std::string(this->base_.get_auth_password()), "hunter2");
  EXPECT_TRUE(auth->is_default());
  EXPECT_EQ(auth->password_length(), 7u);
}

TEST_F(WebAuthTest, AChangeReachesTheServerWithoutARestart) {
  auto auth = this->boot();
  auth->set_credentials("operator", "s3cret-phrase");
  EXPECT_EQ(std::string(this->base_.get_auth_username()), "operator");
  EXPECT_EQ(std::string(this->base_.get_auth_password()), "s3cret-phrase");
  EXPECT_FALSE(auth->is_default());
  EXPECT_EQ(auth->username(), "operator");
  EXPECT_EQ(auth->password_length(), 13u);
}

TEST_F(WebAuthTest, TheStoredPairSurvivesTheNextBoot) {
  auto first = this->boot();
  first->set_credentials("operator", "s3cret-phrase");

  web_server_base::WebServerBase fresh;
  auto second = this->boot(&fresh);
  EXPECT_EQ(std::string(fresh.get_auth_username()), "operator");
  EXPECT_EQ(std::string(fresh.get_auth_password()), "s3cret-phrase");
  EXPECT_FALSE(second->is_default());
}

TEST_F(WebAuthTest, ARecordUnderAnotherKeyIsNotThisComponents) {
  auto first = this->boot();
  first->set_credentials("operator", "s3cret-phrase");

  web_server_base::WebServerBase fresh;
  auto other = this->boot(&fresh, PREF_HASH + 1);
  EXPECT_TRUE(other->is_default());
}

// Setting the pair back to what the firmware was built with says so again, so the dashboard
// and the boot log stop calling a changed device default and start calling this one.
TEST_F(WebAuthTest, TheCompiledPairTypedBackInIsStillTheDefault) {
  auto auth = this->boot();
  auth->set_credentials("operator", "s3cret-phrase");
  ASSERT_FALSE(auth->is_default());
  auth->set_credentials("admin", "hunter2");
  EXPECT_TRUE(auth->is_default());
}

TEST_F(WebAuthTest, TheServerFollowsTheStringsThroughAReallocation) {
  auto auth = this->boot();
  // Long enough to leave any small-string buffer, so the second pair lives somewhere else.
  const std::string longer(PASSWORD_MAX, 'x');
  auth->set_credentials("operator", longer);
  EXPECT_EQ(std::string(this->base_.get_auth_password()), longer);
  auth->set_credentials("operator", "short");
  EXPECT_EQ(std::string(this->base_.get_auth_password()), "short");
}

// The server's task can still be inside authenticate() on the pointers it was last given,
// so the pair it holds must not be the one a change writes over. ASAN reads `held` here.
TEST_F(WebAuthTest, ThePairTheServerHoldsSurvivesAChange) {
  auto auth = this->boot();
  const char *held_user = this->base_.get_auth_username();
  const char *held_pass = this->base_.get_auth_password();
  auth->set_credentials("operator", std::string(PASSWORD_MAX, 'x'));
  EXPECT_EQ(std::string(held_user), "admin");
  EXPECT_EQ(std::string(held_pass), "hunter2");
}

TEST_F(WebAuthTest, AnEmptyFieldIsRefused) {
  EXPECT_STREQ(WebAuth::validate("", "secret"), "'username' is required");
  EXPECT_STREQ(WebAuth::validate("admin", ""), "'password' is required");
}

TEST_F(WebAuthTest, ALongFieldIsRefused) {
  EXPECT_EQ(WebAuth::validate(std::string(USERNAME_MAX, 'u'), "secret"), nullptr);
  EXPECT_STREQ(WebAuth::validate(std::string(USERNAME_MAX + 1, 'u'), "secret"), "'username' is over 32 characters");
  EXPECT_EQ(WebAuth::validate("admin", std::string(PASSWORD_MAX, 'p')), nullptr);
  EXPECT_STREQ(WebAuth::validate("admin", std::string(PASSWORD_MAX + 1, 'p')), "'password' is over 64 characters");
}

// Anything outside printable ASCII cannot survive the header round trip, whichever scheme
// the firmware was built with.
TEST_F(WebAuthTest, ANonAsciiFieldIsRefused) {
  EXPECT_STREQ(WebAuth::validate("админ", "secret"), "'username' must be printable ASCII");
  EXPECT_STREQ(WebAuth::validate("admin", "пароль"), "'password' must be printable ASCII");
  EXPECT_STREQ(WebAuth::validate("adm\tin", "secret"), "'username' must be printable ASCII");
}

// Basic splits the pair on the colon, and a Digest client escapes the quote and the backslash
// in a username the server then reads back raw — a pair that could never match again. A
// password reaches neither header as text, so it may hold all three.
TEST_F(WebAuthTest, AUsernameCarryingASeparatorIsRefused) {
  const char *refusal = "'username' cannot contain ':', '\"' or '\\'";
  EXPECT_STREQ(WebAuth::validate("adm:in", "secret"), refusal);
  EXPECT_STREQ(WebAuth::validate("adm\"in", "secret"), refusal);
  EXPECT_STREQ(WebAuth::validate("adm\\in", "secret"), refusal);
  EXPECT_EQ(WebAuth::validate("admin", "pass:\"word\\"), nullptr);
}

// The record is fixed-width and the strings fill it exactly, so the terminator the loader
// writes back has to land past the last character rather than on it.
TEST_F(WebAuthTest, CredentialsOfTheFullLengthSurviveTheNextBoot) {
  const std::string user(USERNAME_MAX, 'u');
  const std::string pass(PASSWORD_MAX, 'p');
  auto first = this->boot();
  first->set_credentials(user, pass);

  web_server_base::WebServerBase fresh;
  auto second = this->boot(&fresh);
  EXPECT_EQ(std::string(fresh.get_auth_username()), user);
  EXPECT_EQ(std::string(fresh.get_auth_password()), pass);
}

// A record whose username is empty cannot have come from set_credentials, so it is treated as
// no record at all rather than as credentials that would lock the device open.
TEST_F(WebAuthTest, ARecordWithNoUsernameFallsBackToTheCompiledPair) {
  StoredCredentials blank{};
  ESPPreferenceObject pref = global_preferences->make_preference<StoredCredentials>(PREF_HASH);
  ASSERT_TRUE(pref.save(&blank));
  global_preferences->sync();

  auto auth = this->boot();
  EXPECT_EQ(std::string(this->base_.get_auth_username()), "admin");
  EXPECT_TRUE(auth->is_default());
}

TEST_F(WebAuthTest, AStoredPairEqualToTheCompiledOneIsStillTheDefaultAfterABoot) {
  auto first = this->boot();
  first->set_credentials("admin", "hunter2");

  web_server_base::WebServerBase fresh;
  auto second = this->boot(&fresh);
  EXPECT_TRUE(second->is_default());
}

// One read of the live slot, so a route answering from the server's task cannot pair a
// username from one generation with the password length of another.
TEST_F(WebAuthTest, StatusIsTheLiveSlotInOnePiece) {
  auto auth = this->boot();
  WebAuth::Status status = auth->status();
  EXPECT_EQ(status.username, "admin");
  EXPECT_EQ(status.password_length, 7u);
  EXPECT_TRUE(status.is_default);

  auth->set_credentials("operator", "s3cret-phrase");
  status = auth->status();
  EXPECT_EQ(status.username, "operator");
  EXPECT_EQ(status.password_length, 13u);
  EXPECT_FALSE(status.is_default);
}

// The only way back in after a forgotten password, and what the README and the display
// menu's Factory reset promise: the record lives where a reset clears it.
TEST_F(WebAuthTest, AFactoryResetBringsTheCompiledPairBack) {
  auto first = this->boot();
  first->set_credentials("operator", "s3cret-phrase");

  global_preferences->reset();
  global_preferences->sync();

  web_server_base::WebServerBase fresh;
  auto second = this->boot(&fresh);
  EXPECT_EQ(std::string(fresh.get_auth_username()), "admin");
  EXPECT_EQ(std::string(fresh.get_auth_password()), "hunter2");
  EXPECT_TRUE(second->is_default());
}

// The one signal that a device is still reachable with what it left the factory with.
TEST_F(WebAuthTest, TheFactoryPairIsCalledOutAtBootAndThenIsNot) {
  auto first = this->boot();
  EXPECT_TRUE(LogCapture::instance().has("still takes the factory password"));
  first->set_credentials("operator", "s3cret-phrase");

  web_server_base::WebServerBase fresh;
  LogCapture::instance().clear();
  auto second = this->boot(&fresh);
  EXPECT_FALSE(LogCapture::instance().has("still takes the factory password"));
}

TEST_F(WebAuthTest, DumpConfigNamesTheUserAndWhetherThePairIsTheFactoryOne) {
  auto auth = this->boot();
  auth->dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Username: admin"));
  EXPECT_TRUE(LogCapture::instance().has("Password: factory default"));

  auth->set_credentials("operator", "s3cret-phrase");
  LogCapture::instance().clear();
  auth->dump_config();
  EXPECT_TRUE(LogCapture::instance().has("Username: operator"));
  EXPECT_TRUE(LogCapture::instance().has("Password: set"));
  EXPECT_FALSE(LogCapture::instance().has("s3cret-phrase"));
}

TEST_F(WebAuthTest, ARefusedPairLeavesTheServerAlone) {
  auto auth = this->boot();
  auth->set_credentials("adm:in", "secret");
  EXPECT_EQ(std::string(this->base_.get_auth_username()), "admin");
  EXPECT_EQ(std::string(this->base_.get_auth_password()), "hunter2");
}

}  // namespace esphome::web_auth::testing
