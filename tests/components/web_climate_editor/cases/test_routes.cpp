#include "common.h"

// Which requests the handler takes, and what it says before any route runs: the prefix it
// claims, the names it knows, the one method each answers, and who may call it at all.
namespace esphome::web_climate_editor::testing {

TEST_F(Editor, ClaimsEverythingBelowItsPrefixAndNothingElse) {
  for (const char *target : {"/", "/climate-editor-x/api/list", "/climate/Living%20Room", "/files/list",
                             "/automation-editor/api/list", "/climate-editorial"}) {
    AsyncWebServerRequest request(HTTP_GET, target);
    EXPECT_FALSE(this->base.get_server()->dispatch(request)) << target;
  }
  Reply reply = this->get("ping");
  EXPECT_TRUE(reply.claimed);
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, R"({"status":"ok"})");
}

// Claimed and answered, rather than left to the server, which closes the socket on a request
// no handler takes.
TEST_F(Editor, ANameThatIsNoRouteIsNotFound) {
  for (const char *target :
       {"/climate-editor", "/climate-editor/", "/climate-editor/api", "/climate-editor/api/",
        "/climate-editor/api/list/", "/climate-editor/api/listing", "/climate-editor/api/controllers",
        "/climate-editor/api/get/living-room", "/climate-editor/index.html"}) {
    for (http_method method : {HTTP_GET, HTTP_POST}) {
      Reply reply = this->request(method, target);
      EXPECT_TRUE(reply.claimed) << target;
      EXPECT_EQ(reply.code, 404) << target;
      EXPECT_EQ(reply.type, "application/json") << target;
      EXPECT_EQ(reply.error(), "Unknown endpoint") << target;
    }
  }
}

TEST_F(Editor, AnotherPrefixMovesTheRoutes) {
  WebClimateEditor other(&this->base, &hub());
  other.set_url_prefix("/thermostats");
  other.setup();
  AsyncWebServerRequest request(HTTP_GET, "/thermostats/api/ping");
  EXPECT_TRUE(this->base.get_server()->dispatch(request));
  EXPECT_EQ(request.response_code, 200);
}

// The method is checked before anything else the request carries: an id that names no
// thermostat, or none at all, still gets the 405.
TEST_F(Editor, MutatingRoutesArePostOnlyAndTheRestGetOnly) {
  for (const char *route : {"save", "delete?id=nope", "enable?id=nope&value=true", "setpoint?id=nope&value=1"}) {
    Reply reply = this->get(route);
    EXPECT_TRUE(reply.claimed) << route;
    EXPECT_EQ(reply.code, 405) << route;
    EXPECT_EQ(reply.header("Allow"), "POST") << route;
    EXPECT_EQ(reply.error(), "Method not allowed") << route;
  }
  for (const char *route : {"list", "get?id=nope", "status", "entities", "schema", "ping"}) {
    Reply reply = this->post(route);
    EXPECT_EQ(reply.code, 405) << route;
    EXPECT_EQ(reply.header("Allow"), "GET") << route;
  }
  for (http_method method : {HTTP_PUT, HTTP_DELETE}) {
    EXPECT_EQ(this->call(method, "save", LIVING_ROOM).code, 405);
    EXPECT_EQ(this->call(method, "list").code, 405);
  }
  EXPECT_TRUE(this->files().empty());
}

// --- what a page on another site may do with the browser's cached credentials ---

TEST_F(Editor, RefusesACrossSiteWriteAndChangesNothing) {
  const std::string id = this->create(LIVING_ROOM);
  ASSERT_EQ(id, "living-room");
  const char *evil = "http://evil.example";

  for (const auto &route : {std::string("save"), "delete?id=" + id, "enable?id=" + id + "&value=false",
                            "setpoint?id=" + id + "&value=30"}) {
    Reply reply = this->call(HTTP_POST, route, route == "save" ? FLOOR : "", "application/json", evil);
    EXPECT_TRUE(reply.claimed) << route;
    EXPECT_EQ(reply.code, 403) << route;
    EXPECT_EQ(reply.type, "text/plain") << route;
    EXPECT_EQ(reply.body, "Cross-origin request refused") << route;
  }
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
  EXPECT_TRUE(hub().is_running(id));
  EXPECT_FLOAT_EQ(hub().store().get(id)->setpoint, 22.f);
}

TEST_F(Editor, ServesTheDevicesOwnPage) {
  Reply reply = this->call(HTTP_POST, "save", LIVING_ROOM, "application/json", "http://device.local");
  EXPECT_EQ(reply.code, 200) << reply.body;
}

}  // namespace esphome::web_climate_editor::testing
