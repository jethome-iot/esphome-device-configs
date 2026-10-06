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
        "/climate-editor/api/get/living-room", "/climate-editor/index.html", "/climate-editor/api/LIST",
        "/climate-editor/API/list", "/climate-editor/api//list", "/climate-editor//api/list", "/climate-editor/list"}) {
    // Unknown before wrong: no method that reaches the handler makes a name that is no route a 405.
    for (http_method method : {HTTP_GET, HTTP_POST, HTTP_OPTIONS}) {
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
  for (const char *route : {"save", "import", "delete?id=nope", "enable?id=nope&value=true", "setpoint?id=nope&value=1",
                            "preset?id=nope&key=eco"}) {
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
  EXPECT_EQ(this->call(HTTP_OPTIONS, "save", LIVING_ROOM).code, 405);
  EXPECT_EQ(this->call(HTTP_OPTIONS, "import", with(LIVING_ROOM, R"("id":"living-room")")).code, 405);
  EXPECT_EQ(this->call(HTTP_OPTIONS, "list").code, 405);
  EXPECT_TRUE(this->files().empty());
}

// Every route against every method the server passes on, GET, POST and OPTIONS (ESP-IDF answers
// the rest itself): its own one gets past the method check, every other one is the same 405 with
// the one method it takes.
TEST_F(Editor, EveryRouteAnswersItsOneMethodAndRefusesTheRest) {
  struct Case {
    const char *route;
    http_method method;
  };
  for (const Case &c :
       {Case{"list", HTTP_GET}, Case{"get", HTTP_GET}, Case{"status", HTTP_GET}, Case{"entities", HTTP_GET},
        Case{"schema", HTTP_GET}, Case{"ping", HTTP_GET}, Case{"save", HTTP_POST}, Case{"import", HTTP_POST},
        Case{"delete", HTTP_POST}, Case{"enable", HTTP_POST}, Case{"setpoint", HTTP_POST}, Case{"preset", HTTP_POST}}) {
    const char *allow = c.method == HTTP_POST ? "POST" : "GET";
    for (http_method method : {HTTP_GET, HTTP_POST, HTTP_OPTIONS}) {
      Reply reply = this->call(method, c.route);
      EXPECT_TRUE(reply.claimed) << c.route;
      if (method == c.method) {
        EXPECT_NE(reply.code, 405) << c.route << " " << method;
        continue;
      }
      EXPECT_EQ(reply.code, 405) << c.route << " " << method;
      EXPECT_EQ(reply.header("Allow"), allow) << c.route << " " << method;
      EXPECT_EQ(reply.type, "application/json") << c.route << " " << method;
      EXPECT_EQ(reply.error(), "Method not allowed") << c.route << " " << method;
    }
  }
  EXPECT_TRUE(this->files().empty());
}

// The CrossOriginRefuser sits at WIFI and has to be registered first, so it sees every request
// before this handler can claim one.
TEST_F(Editor, RegistersAfterTheCrossOriginRefuser) {
  EXPECT_LT(this->editor->get_setup_priority(), setup_priority::WIFI);
}

// A trivial handler gets no handleBody on the Arduino server, so save would never see a body.
TEST_F(Editor, KeepsItsBodyBecauseTheHandlerIsNotTrivial) { EXPECT_FALSE(this->editor->isRequestHandlerTrivial()); }

TEST_F(Editor, DumpConfigNamesWhereTheApiIs) {
  LogCapture::instance().clear();
  this->editor->dump_config();
  EXPECT_TRUE(LogCapture::instance().has("API: /climate-editor/api/"));

  WebClimateEditor other(&this->base, &hub());
  other.set_url_prefix("/thermostats");
  LogCapture::instance().clear();
  other.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("API: /thermostats/api/"));
}

// --- what a page on another site may do with the browser's cached credentials ---

TEST_F(Editor, RefusesACrossSiteWriteAndChangesNothing) {
  const std::string id = this->create(LIVING_ROOM);
  ASSERT_EQ(id, "living-room");
  const char *evil = "http://evil.example";

  const std::string replacement = with(FLOOR, R"("id":")" + id + "\"");
  for (const auto &route :
       {std::string("save"), std::string("import"), "delete?id=" + id, "enable?id=" + id + "&value=false",
        "setpoint?id=" + id + "&value=30", "preset?id=" + id + "&key=eco"}) {
    const std::string body = route == "save" ? FLOOR : route == "import" ? replacement : "";
    Reply reply = this->call(HTTP_POST, route, body, "application/json", evil);
    EXPECT_TRUE(reply.claimed) << route;
    EXPECT_EQ(reply.code, 403) << route;
    EXPECT_EQ(reply.type, "text/plain") << route;
    EXPECT_EQ(reply.body, "Cross-origin request refused") << route;
  }
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
  EXPECT_TRUE(hub().is_running(id));
  EXPECT_EQ(hub().store().get(id)->name, "Living Room");
  EXPECT_FLOAT_EQ(hub().store().get(id)->setpoint, 22.f);
}

// Reads too: a page on another site has no business learning the thermostats, and the refusal
// comes before the route is looked at, so it costs the loop task nothing.
TEST_F(Editor, RefusesACrossSiteReadBeforeItReachesTheLoopTask) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  hub().jobs = 0;
  for (const char *route :
       {"list", "get?id=living-room", "status", "entities", "schema", "ping", "nothing", "save", "import"}) {
    for (const char *origin : {"http://evil.example", "null", "http://device.local.evil.example"}) {
      Reply reply = this->call(HTTP_GET, route, "", "application/json", origin);
      EXPECT_EQ(reply.code, 403) << route << " from " << origin;
      EXPECT_EQ(reply.body, "Cross-origin request refused") << route << " from " << origin;
    }
  }
  EXPECT_EQ(hub().jobs, 0);
}

// The guard drops a refused request's body before it reaches the editor, so nothing of it is
// left for the next request to read as its own.
TEST_F(Editor, ARefusedCrossSiteBodyIsNotKept) {
  for (const char *route : {"save", "import"}) {
    const std::string body = with(LIVING_ROOM, R"("id":"living-room")");
    Reply refused = this->call(HTTP_POST, route, body, "application/json", "http://evil.example");
    ASSERT_EQ(refused.code, 403) << route;
    Reply form = this->call(HTTP_POST, route, std::string(body.size(), 'x'), "application/x-www-form-urlencoded");
    EXPECT_EQ(form.code, 400) << route;
    EXPECT_EQ(form.error(), "Empty request body") << route;
  }
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Editor, ServesTheDevicesOwnPage) {
  Reply reply = this->call(HTTP_POST, "save", LIVING_ROOM, "application/json", "http://device.local");
  EXPECT_EQ(reply.code, 200) << reply.body;
  reply = this->call(HTTP_POST, "import", with(FLOOR, R"("id":"floor")"), "application/json", "http://device.local");
  EXPECT_EQ(reply.code, 200) << reply.body;
}

}  // namespace esphome::web_climate_editor::testing
