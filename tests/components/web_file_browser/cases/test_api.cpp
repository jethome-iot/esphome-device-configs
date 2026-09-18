#include "common.h"

namespace esphome::web_file_browser::testing {

static const char *const METHOD_NOT_ALLOWED = R"({"success":false,"error":"Method not allowed"})";
// Every filesystem handler is behind USE_ESP32 and answers this off it.
static const char *const NOT_SUPPORTED = R"({"success":false,"error":"Not supported on this platform"})";

TEST_F(Browser, ClaimsEverythingUnderItsPrefix) {
  // The whole prefix, not only the known routes: an unknown tail has to reach handleRequest
  // and be answered, not fall through to whatever handler comes next.
  for (const char *target : {"/files/list", "/files/nothing", "/files/", "/files/list/sub"}) {
    AsyncWebServerRequest request(HTTP_GET, target);
    EXPECT_TRUE(this->browser->canHandle(&request)) << target;
  }
  for (const char *target : {"/", "/files", "/fileslist", "/filesystem/list", "/other/files/list"}) {
    AsyncWebServerRequest request(HTTP_GET, target);
    EXPECT_FALSE(this->browser->canHandle(&request)) << target;
  }
}

TEST_F(Browser, ClaimsNothingWhileTheStorageIsUnmounted) {
  // Never set up, so nothing is mounted: the requests belong to no one rather than to a
  // handler that would answer them off a mount that is not there.
  TestStorage unmounted;
  WebFileBrowser browser(&this->base, &unmounted);
  browser.set_url_prefix("/files");
  ASSERT_FALSE(unmounted.is_mounted());
  for (const char *target : {"/files/list", "/files/info"}) {
    AsyncWebServerRequest request(HTTP_GET, target);
    EXPECT_FALSE(browser.canHandle(&request)) << target;
  }
}

TEST_F(Browser, AnswersAnUnknownRouteWith404) {
  for (const char *target : {"/files/nothing", "/files/", "/files/readme", "/files/list/sub"}) {
    const Reply reply = this->get(target);
    EXPECT_TRUE(reply.claimed) << target;
    EXPECT_EQ(reply.code, 404) << target;
    EXPECT_EQ(reply.type, "text/plain") << target;
    EXPECT_EQ(reply.responses, 1) << target;
  }
}

TEST_F(Browser, RefusesAGetOnAMutatingRoute) {
  // A GET that writes is one <img src> away from being fired by any page the browser opens.
  for (const char *route : {"write", "upload", "delete", "mkdir", "rename", "copy"}) {
    const Reply reply = this->get(std::string("/files/") + route);
    EXPECT_EQ(reply.code, 405) << route;
    EXPECT_EQ(reply.type, "application/json") << route;
    EXPECT_EQ(reply.body, METHOD_NOT_ALLOWED) << route;
    EXPECT_EQ(reply.responses, 1) << route;
  }
}

TEST_F(Browser, RefusesAPostOnAReadOnlyRoute) {
  for (const char *route : {"info", "list", "read", "download"}) {
    const Reply reply = this->post(std::string("/files/") + route);
    EXPECT_EQ(reply.code, 405) << route;
    EXPECT_EQ(reply.body, METHOD_NOT_ALLOWED) << route;
    EXPECT_EQ(reply.responses, 1) << route;
  }
}

TEST_F(Browser, ReportsTheMountOnInfo) {
  const Reply reply = this->get("/files/info");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.type, "application/json");
  EXPECT_EQ(reply.body, R"({"valid":true,"total":131072,"used":4096,"free":126976,"filesystem":"TestFS"})");
  EXPECT_EQ(reply.responses, 1);
}

TEST_F(Browser, ReportsAnUnreadableMountOnInfo) {
  this->storage.info = {};
  EXPECT_EQ(this->get("/files/info").body, R"({"valid":false,"total":0,"used":0,"free":0,"filesystem":"TestFS"})");
}

TEST_F(Browser, LetsTheRightMethodReachEveryHandler) {
  // Off ESP32 the filesystem handlers are a stub, so this says the route resolved and the
  // method check let it through — neither a 404 nor a 405.
  for (const char *route : {"list", "read", "download"}) {
    const Reply reply = this->get(std::string("/files/") + route + "?path=/x");
    EXPECT_EQ(reply.code, 400) << route;
    EXPECT_EQ(reply.body, NOT_SUPPORTED) << route;
    EXPECT_EQ(reply.responses, 1) << route;
  }
  for (const char *route : {"write", "delete", "mkdir", "rename", "copy"}) {
    const Reply reply = this->post(std::string("/files/") + route + "?path=/x");
    EXPECT_EQ(reply.code, 400) << route;
    EXPECT_EQ(reply.body, NOT_SUPPORTED) << route;
    EXPECT_EQ(reply.responses, 1) << route;
  }
}

TEST_F(Browser, RefusesAnUploadThatCarriedNoFile) {
  // The multipart reader skips a zero-length part, so handleUpload never ran: reporting
  // success would lose the file in silence.
  const Reply reply = this->post("/files/upload?path=notes.txt");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.body, R"({"success":false,"error":"No file received"})");
  EXPECT_EQ(reply.responses, 1);
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Browser, WritesAnUploadedFile) {
  const Reply reply = this->upload("/files/upload?path=notes.txt", "notes.txt", "hello");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Upload complete"})");
  EXPECT_EQ(this->contents(this->base_path() + "/notes.txt"), "hello");
}

TEST_F(Browser, RefusesAnUploadOutsideTheMount) {
  const Reply reply = this->upload("/files/upload?path=../escape.txt", "escape.txt", "hello");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.body, R"({"success":false,"error":"Invalid path"})");
  EXPECT_FALSE(this->exists(this->base_path() + "/../escape.txt"));
}

}  // namespace esphome::web_file_browser::testing
