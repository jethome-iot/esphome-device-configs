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
  EXPECT_EQ(reply.error(), "'username' cannot contain ':' or '\"'");
  Dashboard::loop();
  EXPECT_EQ(std::string(this->base.get_auth_username()), "admin");
}

TEST_F(Dashboard, ASetRefusesAnOversizedBody) {
  const std::string body = R"({"username":"admin","password":")" + std::string(5000, 'p') + R"("})";
  Reply reply = this->post("/api/device/auth", body);
  EXPECT_EQ(reply.code, 413);
  EXPECT_EQ(reply.error(), "Request body over 4 KiB");
}

// The route is compiled in with the component, so a firmware whose web_auth failed to come
// up answers 503 rather than a 404 that would read as "this firmware has no credentials".
TEST_F(Dashboard, BothMethodsSayWhenTheComponentIsMissing) {
  web_auth::global_web_auth = nullptr;
  EXPECT_EQ(this->get("/api/device/auth").code, 503);
  EXPECT_EQ(this->post("/api/device/auth", SET_CREDENTIALS).code, 503);
  EXPECT_EQ(this->get("/api/device/auth").error(), "Web auth not available");
  web_auth::global_web_auth = this->auth.get();
}

}  // namespace esphome::web_device_dashboard::testing
