#include "common.h"

namespace esphome::web_file_browser::testing {

// A page on another site, the browser attaching the credentials it has cached for the device.
static const char *const ANOTHER_SITE = "http://evil.example";
static const char *const REFUSED = "Cross-origin request refused";

TEST_F(Browser, RefusesACrossSiteDeleteAndLeavesTheFile) {
  // The delete itself is ESP32-only, so the surviving file is a statement of intent here and
  // not a proof; what this pins down on the host is the answer. The upload case below is the
  // one that really shows a file surviving.
  this->write_file("notes.txt", "keep me");

  const Reply reply = this->call(HTTP_POST, "/files/delete?path=notes.txt", "", ANOTHER_SITE);
  EXPECT_TRUE(reply.claimed);
  EXPECT_EQ(reply.code, 403);
  EXPECT_EQ(reply.type, "text/plain");
  EXPECT_EQ(reply.body, REFUSED);
  EXPECT_EQ(reply.responses, 1);
  EXPECT_EQ(this->contents(this->base_path() + "/notes.txt"), "keep me");
}

TEST_F(Browser, RefusesACrossSiteWriteAndLeavesTheFile) {
  // The one the method check never covered: a form on another site can POST, and this route
  // truncates its target with an empty body. The write itself is ESP32-only, so what this
  // pins down here is the answer and the file; that no byte reaches a handler is in
  // tests/components/web_origin_guard.
  this->write_file("notes.txt", "keep me");

  // Both shapes a form can produce: a text/plain body, and the empty one that truncates.
  for (const std::string &body : {std::string("wiped"), std::string()}) {
    const Reply reply = this->call(HTTP_POST, "/files/write?path=notes.txt", body, ANOTHER_SITE);
    EXPECT_EQ(reply.code, 403) << body;
    EXPECT_EQ(reply.body, REFUSED) << body;
    EXPECT_EQ(reply.responses, 1) << body;
    EXPECT_EQ(this->contents(this->base_path() + "/notes.txt"), "keep me") << body;
  }
}

TEST_F(Browser, RefusesACrossSiteUploadBeforeItOpensTheFile) {
  // handleUpload writes as the parts arrive, so a 403 that only came after would still have
  // overwritten the file.
  this->write_file("notes.txt", "keep me");

  const Reply reply = this->upload("/files/upload?path=notes.txt", "notes.txt", "overwritten", ANOTHER_SITE);
  EXPECT_EQ(reply.code, 403);
  EXPECT_EQ(reply.body, REFUSED);
  EXPECT_EQ(reply.responses, 1);
  EXPECT_EQ(this->contents(this->base_path() + "/notes.txt"), "keep me");
  EXPECT_EQ(this->files().size(), 1u);
}

TEST_F(Browser, RefusesEveryOtherRouteFromAnotherSiteToo) {
  for (const char *route : {"info", "list", "read", "download"}) {
    const Reply reply = this->call(HTTP_GET, std::string("/files/") + route + "?path=/", "", ANOTHER_SITE);
    EXPECT_EQ(reply.code, 403) << route;
    EXPECT_EQ(reply.responses, 1) << route;
  }
  for (const char *route : {"mkdir", "rename", "copy"}) {
    const Reply reply = this->call(HTTP_POST, std::string("/files/") + route + "?path=/x", "", ANOTHER_SITE);
    EXPECT_EQ(reply.code, 403) << route;
    EXPECT_EQ(reply.responses, 1) << route;
  }
}

TEST_F(Browser, ServesTheDevicesOwnPage) {
  // The page fetches with a relative URL, so its Origin is the Host it is addressed to.
  const std::string origin = std::string("http://") + Browser::HOST;
  const Reply info = this->call(HTTP_GET, "/files/info", "", origin.c_str());
  EXPECT_EQ(info.code, 200);
  EXPECT_EQ(info.type, "application/json");

  const Reply reply = this->upload("/files/upload?path=notes.txt", "notes.txt", "hello", origin.c_str());
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(this->contents(this->base_path() + "/notes.txt"), "hello");
}

TEST_F(Browser, ServesAClientThatSendsNoOrigin) {
  // curl and scripts/device-files.py: nothing to judge, nothing refused.
  const Reply reply = this->upload("/files/upload?path=notes.txt", "notes.txt", "hello");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(this->contents(this->base_path() + "/notes.txt"), "hello");
}

}  // namespace esphome::web_file_browser::testing
