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
      {"status", [&] { return this->get("status"); }},
      {"entities", [&] { return this->get("entities"); }},
      {"save", [&] { return this->post("save", FLOOR); }},
      {"save of garbage", [&] { return this->post("save", "garbage"); }},
      {"delete", [&] { return this->post("delete?id=living-room"); }},
      {"enable", [&] { return this->post("enable?id=living-room&value=false"); }},
      {"setpoint", [&] { return this->post("setpoint?id=living-room&value=30"); }},
  };
  // clang-format on
  for (const auto &call : calls) {
    Reply reply = call.second();
    EXPECT_EQ(reply.code, 503) << call.first << ": " << reply.body;
    EXPECT_EQ(reply.error(), "Device busy") << call.first;
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

}  // namespace esphome::web_climate_editor::testing
