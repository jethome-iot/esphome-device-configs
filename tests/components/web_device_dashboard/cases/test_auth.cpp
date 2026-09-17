#include "common.h"

namespace esphome::web_device_dashboard::testing {

static const char *const SET_CREDENTIALS = R"({"username":"operator","password":"s3cret-phrase"})";

TEST_F(Dashboard, AuthReportsTheUsernameAndNeverThePassword) {
  Reply reply = this->get("/api/device/auth");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply["username"].as<std::string>(), "admin");
  EXPECT_EQ(reply["password_length"].as<int>(), 7);
  EXPECT_TRUE(reply["is_default"].as<bool>());
  EXPECT_EQ(reply.body.find("hunter2"), std::string::npos) << reply.body;
}

TEST_F(Dashboard, AuthTakesBothMethods) {
  EXPECT_EQ(this->get("/api/device/auth").code, 200);
  EXPECT_EQ(this->post("/api/device/auth", SET_CREDENTIALS).code, 200);
}

TEST_F(Dashboard, AuthRefusesEveryOtherMethodAndAllowsBoth) {
  for (http_method method : {HTTP_PUT, HTTP_DELETE, HTTP_HEAD}) {
    LogCapture::instance().clear();
    Reply reply = this->call(method, "/api/device/auth");
    EXPECT_EQ(reply.code, 405);
    EXPECT_TRUE(LogCapture::instance().has_warning("allowed: GET, POST"));
  }
}

// The answer goes out before the change, so a client is told what happened rather than
// losing the connection that told it.
TEST_F(Dashboard, ASetIsAnsweredBeforeItIsApplied) {
  Reply reply = this->post("/api/device/auth", SET_CREDENTIALS);
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Credentials updated");
  EXPECT_EQ(std::string(this->base.get_auth_username()), "admin");

  Dashboard::loop();
  EXPECT_EQ(std::string(this->base.get_auth_username()), "operator");
  EXPECT_EQ(std::string(this->base.get_auth_password()), "s3cret-phrase");
}

TEST_F(Dashboard, AuthReportsWhatWasSet) {
  ASSERT_EQ(this->post("/api/device/auth", SET_CREDENTIALS).code, 200);
  Dashboard::loop();
  Reply reply = this->get("/api/device/auth");
  EXPECT_EQ(reply["username"].as<std::string>(), "operator");
  EXPECT_EQ(reply["password_length"].as<int>(), 13);
  EXPECT_FALSE(reply["is_default"].as<bool>());
}

TEST_F(Dashboard, ASetRefusesABodyThatIsNotAnObject) {
  for (const char *body : {"", "not json", "[]", "42"}) {
    Reply reply = this->post("/api/device/auth", body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "Invalid JSON") << body;
  }
}

// A number or a null read as an empty string would shut the device behind credentials
// nobody typed, so the type is checked before the value.
TEST_F(Dashboard, ASetRefusesAFieldThatIsNotAString) {
  for (const char *body :
       {R"({"password":"secret"})", R"({"username":null,"password":"secret"})", R"({"username":1,"password":"secret"})",
        R"({"username":"admin"})", R"({"username":"admin","password":true})"}) {
    Reply reply = this->post("/api/device/auth", body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "'username' and 'password' must be strings") << body;
  }
}

TEST_F(Dashboard, ASetPassesTheComponentsRefusalOn) {
  Reply reply = this->post("/api/device/auth", R"({"username":"adm:in","password":"secret"})");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "'username' cannot contain ':', '\"' or '\\'");
  Dashboard::loop();
  EXPECT_EQ(std::string(this->base.get_auth_username()), "admin");
}

// text/plain is the only one of the three form encodings the server hands on as a raw body,
// so it is the one a cross-site form would arrive as; the type check is what keeps it out.
TEST_F(Dashboard, ASetRefusesABodyThatDoesNotSayItIsJson) {
  for (const char *type : {"text/plain", "text/plain;charset=UTF-8", ""}) {
    Reply reply = this->call(HTTP_POST, "/api/device/auth", SET_CREDENTIALS, 512, type);
    EXPECT_EQ(reply.code, 415) << type;
    EXPECT_EQ(reply.error(), "Expected Content-Type: application/json") << type;
  }
  Dashboard::loop();
  EXPECT_EQ(std::string(this->base.get_auth_username()), "admin");
}

// A media type is case-insensitive and may carry parameters, and upstream reads the form
// types the same way; a client that spells it either way is not a client to turn away.
TEST_F(Dashboard, ASetTakesTheTypeWithParametersAndInAnyCase) {
  for (const char *type : {"application/json; charset=utf-8", "Application/JSON"}) {
    Reply reply = this->call(HTTP_POST, "/api/device/auth", SET_CREDENTIALS, 512, type);
    EXPECT_EQ(reply.code, 200) << type;
  }
  Dashboard::loop();
  EXPECT_EQ(std::string(this->base.get_auth_username()), "operator");
}

// Two posts before the loop runs must leave one pending publish, not two. The component
// alternates two slots, so a second publish in the same turn writes back over the first —
// the very strings a request still inside authenticate() is reading.
TEST_F(Dashboard, ASecondSetBeforeTheLoopReplacesTheFirst) {
  const char *held_user = this->base.get_auth_username();
  const char *held_pass = this->base.get_auth_password();
  ASSERT_EQ(this->post("/api/device/auth", SET_CREDENTIALS).code, 200);
  ASSERT_EQ(this->post("/api/device/auth", R"({"username":"second","password":"other-phrase"})").code, 200);
  Dashboard::loop();

  EXPECT_EQ(std::string(this->base.get_auth_username()), "second");
  EXPECT_EQ(std::string(this->base.get_auth_password()), "other-phrase");
  EXPECT_EQ(std::string(held_user), "admin");
  EXPECT_EQ(std::string(held_pass), "hunter2");
}

TEST_F(Dashboard, ASetRefusesAnOversizedBody) {
  const std::string body = R"({"username":"admin","password":")" + std::string(5000, 'p') + R"("})";
  Reply reply = this->post("/api/device/auth", body);
  EXPECT_EQ(reply.code, 413);
  EXPECT_EQ(reply.error(), "Request body over 4 KiB");
}

// The guard the entity-settings routes also carry: the route exists wherever the component is
// compiled in, and a missing component is a 503, not a 404 that would read as "this firmware
// has no credentials". Codegen constructs WebAuth before any handler registers, so nothing on
// a device reaches this — the test nulls the pointer by hand.
TEST_F(Dashboard, BothMethodsSayWhenTheComponentIsMissing) {
  web_auth::global_web_auth = nullptr;
  EXPECT_EQ(this->get("/api/device/auth").code, 503);
  EXPECT_EQ(this->post("/api/device/auth", SET_CREDENTIALS).code, 503);
  EXPECT_EQ(this->get("/api/device/auth").error(), "Web auth not available");
  web_auth::global_web_auth = this->auth.get();
}

}  // namespace esphome::web_device_dashboard::testing
