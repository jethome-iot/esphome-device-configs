#include "common.h"

// The documents, the running thermostats and the relay claims belong to the loop task: a Home
// Assistant call, a sensor sample and the hub's own tick all change them there. A request
// arrives on the HTTP server's own task, so every route that touches them hands its whole read
// -- and whatever it decides from it -- over in one job, and answers from what came back.
namespace esphome::web_climate_editor::testing {

TEST_F(Editor, EveryHubRouteGoesOverToTheLoopTaskOnce) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  struct Call {
    const char *what;
    std::function<Reply()> run;
  };
  // The writes last, delete at the very end: each leaves the hub different from before.
  // clang-format off
  const std::vector<Call> calls = {
      {"list", [&] { return this->get("list"); }},
      {"get", [&] { return this->get("get?id=living-room"); }},
      {"status", [&] { return this->get("status"); }},
      {"status of one", [&] { return this->get("status?id=living-room"); }},
      {"entities", [&] { return this->get("entities"); }},
      {"save", [&] { return this->post("save", FLOOR); }},
      {"setpoint", [&] { return this->post("setpoint?id=living-room&value=20"); }},
      {"enable", [&] { return this->post("enable?id=living-room&value=false"); }},
      {"delete", [&] { return this->post("delete?id=living-room"); }},
  };
  // clang-format on
  for (const Call &call : calls) {
    hub().jobs = 0;
    Reply reply = call.run();
    EXPECT_EQ(reply.code, 200) << call.what << ": " << reply.body;
    EXPECT_EQ(hub().jobs, 1) << call.what;
  }
}

// Build-time data only: the parameter table and a literal. Holding the loop task up for them
// would be a cost for no reason.
TEST_F(Editor, TheRoutesThatTouchNoHubStateStayOnTheServerTask) {
  for (const char *route : {"schema", "ping"}) {
    hub().jobs = 0;
    EXPECT_EQ(this->get(route).code, 200) << route;
    EXPECT_EQ(hub().jobs, 0) << route << " went over to the loop task for nothing";
  }
}

// What the request itself gets wrong is answered where it arrives.
TEST_F(Editor, ARequestRefusedOnItsOwnCostsNoJob) {
  hub().jobs = 0;
  for (const char *route : {"get", "get?id=Bad", "status?id=", "delete", "enable?id=a", "enable?id=a&value=x",
                            "setpoint?id=a&value=x", "nothing"}) {
    const bool post = std::string(route).rfind("get", 0) != 0 && std::string(route).rfind("status", 0) != 0;
    Reply reply = post ? this->post(route) : this->get(route);
    EXPECT_NE(reply.code, 200) << route;
  }
  EXPECT_EQ(this->post("save").code, 400);
  EXPECT_EQ(this->get("save").code, 405);
  EXPECT_EQ(this->post("save", std::string(9000, 'x')).code, 413);
  EXPECT_EQ(hub().jobs, 0);
}

// 503, not the 500 an unknown status turns into on ESP-IDF: nothing was read or written, and
// the call can simply be made again.
TEST_F(Editor, ALoopThatNeverTakesTheJobAnswersBusyAndChangesNothing) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  const std::vector<std::string> before = this->files();

  hub().loop_busy = true;
  // clang-format off
  const std::vector<std::pair<const char *, std::function<Reply()>>> calls = {
      {"list", [&] { return this->get("list"); }},
      {"get", [&] { return this->get("get?id=living-room"); }},
      {"get of a ghost", [&] { return this->get("get?id=ghost"); }},
      {"status", [&] { return this->get("status"); }},
      {"status of one", [&] { return this->get("status?id=living-room"); }},
      {"entities", [&] { return this->get("entities"); }},
      {"save", [&] { return this->post("save", FLOOR); }},
      {"save of garbage", [&] { return this->post("save", "garbage"); }},
      {"update", [&] { return this->post("save", R"({"id":"living-room","name":"Lounge"})"); }},
      {"delete", [&] { return this->post("delete?id=living-room"); }},
      {"enable", [&] { return this->post("enable?id=living-room&value=false"); }},
      {"take over", [&] { return this->post("enable?id=living-room&value=true&take_over=true"); }},
      {"setpoint", [&] { return this->post("setpoint?id=living-room&value=30"); }},
  };
  // clang-format on
  for (const auto &call : calls) {
    hub().jobs = 0;
    Reply reply = call.second();
    EXPECT_EQ(reply.code, 503) << call.first << ": " << reply.body;
    EXPECT_EQ(reply.type, "application/json") << call.first;
    EXPECT_EQ(reply.error(), "Device busy") << call.first;
    EXPECT_EQ(hub().jobs, 1) << call.first << " was not handed over exactly once";
  }
  hub().loop_busy = false;

  // Refused, not half done: the thermostat, its file and its target are as they were.
  EXPECT_EQ(this->files(), before);
  EXPECT_EQ(hub().store().size(), 1u);
  EXPECT_TRUE(hub().is_running("living-room"));
  EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, 22.f);
}

// A refusal raised inside the job is the job's answer, not the loop failing to run it.
TEST_F(Editor, ARefusalFoundOnTheLoopTaskKeepsItsOwnStatus) {
  for (const auto &reply : {this->get("get?id=ghost"), this->get("status?id=ghost"), this->post("delete?id=ghost"),
                            this->post("enable?id=ghost&value=true"), this->post("setpoint?id=ghost&value=20")}) {
    EXPECT_EQ(reply.code, 404) << reply.body;
  }
  EXPECT_EQ(this->post("save", "garbage").code, 400);
}

// A hub whose storage never mounted runs its jobs in place, since no loop schedules it: the
// reads still answer, and every write is refused with the same 500.
TEST_F(Editor, AHubWithoutStorageStillAnswersReadsAndRefusesEveryWrite) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  hub().mark_failed();

  Reply list = this->get("list");
  EXPECT_EQ(list.code, 200) << list.body;
  EXPECT_EQ(list["count"].as<int>(), 1);
  for (const char *route : {"get?id=living-room", "status", "status?id=living-room", "entities", "schema"})
    EXPECT_EQ(this->get(route).code, 200) << route;

  for (Reply reply : {this->post("save", FLOOR), this->post("save", with(LIVING_ROOM, R"("id":"living-room")")),
                      this->post("delete?id=living-room"), this->post("enable?id=living-room&value=false"),
                      this->post("setpoint?id=living-room&value=30")}) {
    EXPECT_EQ(reply.code, 500) << reply.body;
    EXPECT_EQ(reply.error(), "Thermostat storage is not available");
  }
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
  EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, 22.f);
}

// A hub that failed at boot loaded nothing: an id it does not know is still the storage's 500.
TEST_F(Editor, AHubWithoutStorageRefusesAWriteToAnyIdWith500) {
  hub().mark_failed();
  for (Reply reply : {this->post("enable?id=ghost&value=true"), this->post("enable?id=ghost&value=false"),
                      this->post("delete?id=ghost"), this->post("setpoint?id=ghost&value=20")}) {
    EXPECT_EQ(reply.code, 500) << reply.body;
    EXPECT_EQ(reply.error(), "Thermostat storage is not available") << reply.body;
  }
  // What the request itself gets wrong is still its answer.
  EXPECT_EQ(this->post("enable?id=ghost").error(), "Missing value parameter");
}

// In place means on the server task, whose stack is small: a save is refused before its body
// is parsed, however broken the body is.
TEST_F(Editor, AHubWithoutStorageRefusesASaveBeforeParsingIt) {
  hub().mark_failed();
  hub().jobs = 0;
  for (const std::string &body : {std::string(FLOOR), std::string("garbage"), with(LIVING_ROOM, R"("id":"ghost")")}) {
    Reply reply = this->post("save", body);
    EXPECT_EQ(reply.code, 500) << body;
    EXPECT_EQ(reply.error(), "Thermostat storage is not available") << body;
  }
  EXPECT_EQ(hub().jobs, 0);
  // What the request itself gets wrong is still its answer.
  EXPECT_EQ(this->post("save").error(), "Empty request body");
  EXPECT_EQ(this->post("save", std::string(9000, 'x')).code, 413);
  EXPECT_TRUE(this->files().empty());
}

}  // namespace esphome::web_climate_editor::testing
