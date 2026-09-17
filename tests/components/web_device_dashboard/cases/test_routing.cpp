#include "common.h"
#include "esphome/components/web_device_dashboard/dashboard_index.h"

namespace esphome::web_device_dashboard::testing {

TEST_F(Dashboard, ClaimsThePageAndEverythingUnderTheApiPrefix) {
  EXPECT_TRUE(this->claims("/"));
  EXPECT_TRUE(this->claims("/api/device/info"));
  // Claimed too, so an unknown route is answered by this API and not by web_server's 404.
  EXPECT_TRUE(this->claims("/api/device/nonesuch"));
  EXPECT_TRUE(this->claims("/api/device/"));
}

TEST_F(Dashboard, LeavesEveryOtherUrlToItsOwnHandler) {
  for (const char *url :
       {"/files", "/automation-editor/api/ping", "/api/other", "/api/device", "/index.html", "/switch/relay_1", ""}) {
    EXPECT_FALSE(this->claims(url)) << url;
  }
}

TEST_F(Dashboard, LooksEveryRouteUpByItsName) {
  const Route *info = this->dashboard->route_for_("/api/device/info");
  ASSERT_NE(info, nullptr);
  EXPECT_EQ(info->id, RouteId::INFO);
  EXPECT_EQ(this->dashboard->route_for_("/api/device/status")->id, RouteId::STATUS);
  EXPECT_EQ(this->dashboard->route_for_("/api/device/network")->id, RouteId::NETWORK);
  EXPECT_EQ(this->dashboard->route_for_("/api/device/entities")->id, RouteId::ENTITIES);
  EXPECT_EQ(this->dashboard->route_for_("/api/device/entity-settings")->id, RouteId::ENTITY_SETTINGS);
  EXPECT_EQ(this->dashboard->route_for_("/api/device/entity-settings-meta")->id, RouteId::ENTITY_SETTINGS_META);
}

TEST_F(Dashboard, HasNoRouteForAnythingElse) {
  // The tail is matched whole: no prefix, no suffix and no path below a route.
  for (const char *url : {"/", "/api/device/", "/api/device/nonesuch", "/api/device/inf", "/api/device/information",
                          "/api/device/info/extra", "/api/device/entity-settings/meta", "/info"}) {
    EXPECT_EQ(this->dashboard->route_for_(url), nullptr) << url;
  }
}

TEST_F(Dashboard, ReadRoutesAnswerGet) {
  for (const char *url : {"/api/device/info", "/api/device/status", "/api/device/network", "/api/device/entities",
                          "/api/device/entity-settings-meta"}) {
    Reply reply = this->get(url);
    EXPECT_EQ(reply.code, 200) << url;
    EXPECT_EQ(reply.type, "application/json") << url;
  }
}

TEST_F(Dashboard, ReadRoutesRefusePostAndAllowOnlyGet) {
  for (const char *url : {"/api/device/info", "/api/device/status", "/api/device/network", "/api/device/entities",
                          "/api/device/entity-settings-meta"}) {
    LogCapture::instance().clear();
    Reply reply = this->post(url);
    EXPECT_EQ(reply.code, 405) << url;
    EXPECT_FALSE(reply.success()) << url;
    EXPECT_EQ(reply.error(), "Method not allowed") << url;
    // The Allow value only reaches a header on ESP-IDF; here the warning carries it.
    EXPECT_TRUE(LogCapture::instance().has_warning("allowed: GET")) << url;
  }
}

TEST_F(Dashboard, EntitySettingsTakesBothMethods) {
  Reply read = this->get("/api/device/entity-settings?type=switch");
  EXPECT_EQ(read.code, 200);
  Reply write = this->post("/api/device/entity-settings", UPDATE_RELAY_1);
  EXPECT_EQ(write.code, 200);
}

TEST_F(Dashboard, EntitySettingsRefusesEveryOtherMethodAndAllowsBoth) {
  for (http_method method : {HTTP_PUT, HTTP_DELETE, HTTP_HEAD}) {
    LogCapture::instance().clear();
    Reply reply = this->call(method, "/api/device/entity-settings");
    EXPECT_EQ(reply.code, 405) << method;
    EXPECT_TRUE(LogCapture::instance().has_warning("allowed: GET, POST")) << method;
  }
}

TEST_F(Dashboard, AnUnknownRouteIsThisApisNotFound) {
  Reply reply = this->get("/api/device/nonesuch");
  EXPECT_TRUE(reply.claimed);
  EXPECT_EQ(reply.code, 404);
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "Not found");
}

TEST_F(Dashboard, AnUnknownRouteIsNotFoundWhateverTheMethod) {
  // The name is checked before the method, so an unknown route never answers 405.
  Reply reply = this->post("/api/device/nonesuch", "{}");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Not found");
}

TEST_F(Dashboard, ServesTheGzippedPageAtTheRoot) {
  Reply reply = this->get("/");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.type, "text/html");
  EXPECT_EQ(reply.header("Content-Encoding"), "gzip");
  EXPECT_EQ(reply.header("Cache-Control"), "public, max-age=3600");
  EXPECT_EQ(reply.body.size(), sizeof(INDEX_GZ));
  EXPECT_EQ(reply.body.compare(0, 2, "\x1f\x8b"), 0);
}

TEST_F(Dashboard, TheRootTakesGetOnly) {
  Reply reply = this->post("/");
  EXPECT_EQ(reply.code, 405);
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "Method not allowed");
}

TEST_F(Dashboard, RegistersAheadOfTheWebServerIndexPage) {
  // web_server's handler sits at WIFI and would otherwise claim / first.
  EXPECT_LT(this->dashboard->get_setup_priority(), setup_priority::WIFI);
}

TEST_F(Dashboard, KeepsItsBodyBecauseTheHandlerIsNotTrivial) {
  // A trivial handler gets no handleBody at all, so the POST routes would never see a body.
  EXPECT_FALSE(this->dashboard->isRequestHandlerTrivial());
}

}  // namespace esphome::web_device_dashboard::testing
