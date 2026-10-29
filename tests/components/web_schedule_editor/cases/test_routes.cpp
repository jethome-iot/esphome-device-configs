#include "common.h"

// Which requests the handler takes, and what it says before any route runs: the prefix it
// claims, the names it knows, the one method each answers, the body it reads, and who may call
// it at all.
namespace esphome::web_schedule_editor::testing {

TEST_F(Editor, ClaimsEverythingBelowItsPrefixAndNothingElse) {
  for (const char *target : {"/", "/schedule-editor-x/api/list", "/switch/relay_1", "/files/list",
                             "/climate-editor/api/list", "/schedule-editorial"}) {
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
  for (const char *target : {"/schedule-editor", "/schedule-editor/", "/schedule-editor/api", "/schedule-editor/api/",
                             "/schedule-editor/api/list/", "/schedule-editor/api/listing",
                             "/schedule-editor/api/status", "/schedule-editor/api/get/porch",
                             "/schedule-editor/index.html", "/schedule-editor/api/LIST", "/schedule-editor/API/list",
                             "/schedule-editor/api//list", "/schedule-editor//api/list", "/schedule-editor/list"}) {
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
  WebScheduleEditor other(&this->base, &engine());
  other.set_url_prefix("/timetables");
  other.setup();
  AsyncWebServerRequest request(HTTP_GET, "/timetables/api/ping");
  EXPECT_TRUE(this->base.get_server()->dispatch(request));
  EXPECT_EQ(request.response_code, 200);
}

// Every route against every method the server passes on, GET, POST and OPTIONS (ESP-IDF answers
// the rest itself): its own one gets past the method check, every other one is the same 405 with
// the one method it takes, before an id that names no schedule, or none at all, is looked at.
TEST_F(Editor, EveryRouteAnswersItsOneMethodAndRefusesTheRest) {
  struct Case {
    const char *route;
    http_method method;
  };
  for (const Case &c : {Case{"list", HTTP_GET}, Case{"get", HTTP_GET}, Case{"targets", HTTP_GET},
                        Case{"schema", HTTP_GET}, Case{"ping", HTTP_GET}, Case{"save", HTTP_POST},
                        Case{"import", HTTP_POST}, Case{"delete", HTTP_POST}, Case{"enable", HTTP_POST}}) {
    const char *allow = c.method == HTTP_POST ? "POST" : "GET";
    for (http_method method : {HTTP_GET, HTTP_POST, HTTP_OPTIONS}) {
      Reply reply = this->call(method, c.route, method == HTTP_GET ? "" : porch());
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
  // The one save above with its own method created the porch; nothing else wrote.
  EXPECT_EQ(this->files(), std::vector<std::string>{"porch.json"});
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
  EXPECT_TRUE(LogCapture::instance().has("API: /schedule-editor/api/"));

  WebScheduleEditor other(&this->base, &engine());
  other.set_url_prefix("/timetables");
  LogCapture::instance().clear();
  other.dump_config();
  EXPECT_TRUE(LogCapture::instance().has("API: /timetables/api/"));
}

// --- the parameters, each answered before the loop task is asked for anything ---

TEST_F(Editor, AnIdIsASlugAndItsAbsenceIsSaid) {
  engine().jobs = 0;
  for (const char *route : {"get", "get?id=", "get?id=Porch", "get?id=a--b", "get?id=porch%20light"}) {
    Reply reply = this->get(route);
    EXPECT_EQ(reply.code, 400) << route;
    EXPECT_EQ(reply.error(), std::string(route) == "get" ? "Missing id parameter" : "Invalid id parameter") << route;
  }
  for (const char *route : {"delete", "enable?value=true"}) {
    Reply reply = this->post(route);
    EXPECT_EQ(reply.code, 400) << route;
    EXPECT_EQ(reply.error(), "Missing id parameter") << route;
  }
  EXPECT_EQ(this->post("delete?id=../x").error(), "Invalid id parameter");
  EXPECT_EQ(engine().jobs, 0);
}

TEST_F(Editor, EnableTakesTrueOrFalseAndNothingElse) {
  ASSERT_EQ(this->create(porch()), "porch");
  engine().jobs = 0;
  EXPECT_EQ(this->post("enable?id=porch").error(), "Missing value parameter");
  for (const char *value : {"1", "0", "yes", "TRUE", ""}) {
    Reply reply = this->post(std::string("enable?id=porch&value=") + value);
    EXPECT_EQ(reply.code, 400) << value;
    EXPECT_EQ(reply.error(), "Invalid value parameter") << value;
    reply = this->post(std::string("enable?id=porch&value=true&take_over=") + value);
    EXPECT_EQ(reply.error(), "Invalid take_over parameter") << value;
  }
  EXPECT_EQ(engine().jobs, 0);
}

// --- the body of save and import ---

TEST_F(Editor, ABodyIsReadWhole) {
  for (const char *route : {"save", "import"}) {
    EXPECT_EQ(this->post(route, "").error(), "Empty request body") << route;
    EXPECT_EQ(this->post(route, "  ").error(), "JSON parse error: EmptyInput") << route;
    EXPECT_EQ(this->post(route, "{").error(), "JSON parse error: IncompleteInput") << route;
    EXPECT_EQ(this->post(route, "nope").error(), "JSON parse error: InvalidInput") << route;
    EXPECT_EQ(this->post(route, "42").error(), "document is not an object") << route;
  }
  EXPECT_TRUE(this->files().empty());
}

// A form is parsed into parameters by the server, so nothing of it reaches the editor's body.
TEST_F(Editor, AFormBodyIsNoDocument) {
  Reply reply = this->call(HTTP_POST, "save", "name=Porch", "application/x-www-form-urlencoded");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Empty request body");
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Editor, ABodyOver16KiBIsRefusedUnreadAndTheNextRequestStartsClean) {
  engine().jobs = 0;
  std::string big = porch();
  big.resize(16385, ' ');
  for (const char *route : {"save", "import"}) {
    Reply reply = this->post(route, big);
    EXPECT_EQ(reply.code, 413) << route;
    EXPECT_EQ(reply.error(), "Request body over 16 KiB") << route;
  }
  EXPECT_EQ(engine().jobs, 0) << "refused before the loop task is asked";
  EXPECT_TRUE(this->files().empty());
  // The buffer was this request's alone.
  EXPECT_EQ(this->post("save").error(), "Empty request body");
  big.resize(16384);
  EXPECT_EQ(this->post("save", big).code, 200);
}

// --- the loop task ---

TEST_F(Editor, EveryRouteButSchemaAndPingCrossesToTheLoopTaskOnce) {
  ASSERT_EQ(this->create(porch()), "porch");
  for (const char *route : {"list", "get?id=porch", "targets"}) {
    engine().jobs = 0;
    EXPECT_EQ(this->get(route).code, 200) << route;
    EXPECT_EQ(engine().jobs, 1) << route;
  }
  for (const char *route : {"schema", "ping"}) {
    engine().jobs = 0;
    EXPECT_EQ(this->get(route).code, 200) << route;
    EXPECT_EQ(engine().jobs, 0) << route;
  }
  for (const char *route : {"enable?id=porch&value=false", "delete?id=porch"}) {
    engine().jobs = 0;
    EXPECT_EQ(this->post(route).code, 200) << route;
    EXPECT_EQ(engine().jobs, 1) << route;
  }
  engine().jobs = 0;
  EXPECT_EQ(this->post("save", porch()).code, 200);
  EXPECT_EQ(engine().jobs, 1);
}

TEST_F(Editor, ALoopThatNeverTakesTheJobIs503AndChangesNothing) {
  ASSERT_EQ(this->create(porch()), "porch");
  const std::string before = this->file("porch.json");
  engine().loop_busy = true;
  for (const char *route : {"list", "get?id=porch", "targets"}) {
    Reply reply = this->get(route);
    EXPECT_EQ(reply.code, 503) << route;
    EXPECT_EQ(reply.error(), "Device busy") << route;
  }
  for (const auto &[route, body] :
       std::vector<std::pair<std::string, std::string>>{{"save", with(porch("Porch light"), R"("id":"porch")")},
                                                        {"import", with(porch("Porch light"), R"("id":"porch")")},
                                                        {"enable?id=porch&value=false", ""},
                                                        {"delete?id=porch", ""}}) {
    EXPECT_EQ(this->post(route, body).code, 503) << route;
  }
  // Build-time data never waits.
  EXPECT_EQ(this->get("schema").code, 200);
  EXPECT_EQ(this->get("ping").code, 200);
  engine().loop_busy = false;
  EXPECT_EQ(this->file("porch.json"), before);
}

// The engine runs a job in place once it failed, on the server task's small stack: a body is
// refused before it is parsed, and a read still answers.
TEST_F(Editor, StorageThatFailedAtBootRefusesEveryWriteBeforeReadingTheBody) {
  engine().mark_failed();
  engine().jobs = 0;
  for (const char *route : {"save", "import"}) {
    Reply reply = this->post(route, "not even JSON");
    EXPECT_EQ(reply.code, 500) << route;
    EXPECT_EQ(reply.error(), "Schedule storage is not available") << route;
  }
  EXPECT_EQ(engine().jobs, 0);
  for (const char *route : {"delete?id=porch", "enable?id=porch&value=true"}) {
    Reply reply = this->post(route);
    EXPECT_EQ(reply.code, 500) << route;
    EXPECT_EQ(reply.error(), "Schedule storage is not available") << route;
  }
  Reply list = this->get("list");
  EXPECT_EQ(list.code, 200);
  EXPECT_EQ(list["count"].as<int>(), 0);
  engine().reset_to_construction_state();
}

// --- what a page on another site may do with the browser's cached credentials ---

TEST_F(Editor, RefusesACrossSiteWriteAndChangesNothing) {
  ASSERT_EQ(this->create(porch()), "porch");
  const std::string before = this->file("porch.json");
  const char *evil = "http://evil.example";
  for (const auto &[route, body] :
       std::vector<std::pair<std::string, std::string>>{{"save", with(porch("Porch light"), R"("id":"porch")")},
                                                        {"import", with(porch("Porch light"), R"("id":"porch")")},
                                                        {"enable?id=porch&value=false", ""},
                                                        {"delete?id=porch", ""}}) {
    Reply reply = this->call(HTTP_POST, route, body, "application/json", evil);
    EXPECT_TRUE(reply.claimed) << route;
    EXPECT_EQ(reply.code, 403) << route;
    EXPECT_EQ(reply.type, "text/plain") << route;
    EXPECT_EQ(reply.body, "Cross-origin request refused") << route;
  }
  EXPECT_EQ(this->files(), std::vector<std::string>{"porch.json"});
  EXPECT_EQ(this->file("porch.json"), before);
}

// Reads too: a page on another site has no business learning the schedules, and the refusal
// comes before the route is looked at, so it costs the loop task nothing.
TEST_F(Editor, RefusesACrossSiteReadBeforeItReachesTheLoopTask) {
  ASSERT_EQ(this->create(porch()), "porch");
  engine().jobs = 0;
  for (const char *route : {"list", "get?id=porch", "targets", "schema", "ping", "nothing", "save"}) {
    for (const char *origin : {"http://evil.example", "null", "http://device.local.evil.example"}) {
      Reply reply = this->call(HTTP_GET, route, "", "application/json", origin);
      EXPECT_EQ(reply.code, 403) << route << " from " << origin;
      EXPECT_EQ(reply.body, "Cross-origin request refused") << route << " from " << origin;
    }
  }
  EXPECT_EQ(engine().jobs, 0);
}

// The guard drops a refused request's body before it reaches the editor, so nothing of it is
// left for the next request to read as its own.
TEST_F(Editor, ARefusedCrossSiteBodyIsNotKept) {
  for (const char *route : {"save", "import"}) {
    const std::string body = with(porch(), R"("id":"porch")");
    Reply refused = this->call(HTTP_POST, route, body, "application/json", "http://evil.example");
    ASSERT_EQ(refused.code, 403) << route;
    Reply form = this->call(HTTP_POST, route, std::string(body.size(), 'x'), "application/x-www-form-urlencoded");
    EXPECT_EQ(form.code, 400) << route;
    EXPECT_EQ(form.error(), "Empty request body") << route;
  }
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Editor, ServesTheDevicesOwnPage) {
  Reply reply = this->call(HTTP_POST, "save", porch(), "application/json", "http://device.local");
  EXPECT_EQ(reply.code, 200) << reply.body;
  reply = this->call(HTTP_POST, "import", with(porch("Lamp", "relay_1"), R"("id":"lamp")"), "application/json",
                     "http://device.local");
  EXPECT_EQ(reply.code, 200) << reply.body;
}

}  // namespace esphome::web_schedule_editor::testing
