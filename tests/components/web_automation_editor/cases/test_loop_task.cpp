#include "common.h"

// The rule list belongs to the loop task: a lambda may call add_automation() or
// remove_automation() there, and either reallocates the vector these routes walk. A request
// arrives on the HTTP server's own task, so every route that touches the list hands its whole
// read -- and whatever it decides from it -- over, and answers from what came back.
namespace esphome::web_automation_editor::testing {

TEST_F(Editor, EveryRuleRouteGoesOverToTheLoopTask) {
  const uint32_t id = this->create(PORCH_LIGHT);
  ASSERT_NE(id, 0u);
  const std::string one = "get?id=" + std::to_string(id);
  const std::string gone = "delete?id=" + std::to_string(id);

  struct Call {
    const char *what;
    std::function<Reply()> run;
  };
  // Save and delete last: each leaves the list a rule different from the one before it.
  // clang-format off
  const std::vector<Call> calls = {
      {"list", [&] { return this->get("list"); }},
      {"get", [&] { return this->get(one); }},
      {"export", [&] { return this->get("export"); }},
      {"save", [&] { return this->post("save", FAN); }},
      {"delete", [&] { return this->post(gone); }},
  };
  // clang-format on
  for (const Call &call : calls) {
    this->engine->jobs = 0;
    Reply reply = call.run();
    EXPECT_EQ(reply.code, 200) << call.what << ": " << reply.body;
    EXPECT_EQ(this->engine->jobs, 1) << call.what << " read the rule list on the server task";
  }
}

// The routes that answer from what the build already fixed: App's entity lists and a string
// literal. Nothing there is the loop task's, and holding it up for them would be a cost for
// no reason -- the editor polls entities alongside the list.
TEST_F(Editor, TheRoutesThatTouchNoRuleStayOnTheServerTask) {
  for (const char *route : {"entities", "schema", "ping"}) {
    this->engine->jobs = 0;
    EXPECT_EQ(this->get(route).code, 200) << route;
    EXPECT_EQ(this->engine->jobs, 0) << route << " went over to the loop task for nothing";
  }
}

// 503, not the 500 an unknown status turns into on ESP-IDF: nothing was read or written, and
// the call can simply be made again.
TEST_F(Editor, ALoopThatNeverTakesTheJobAnswersBusyAndChangesNothing) {
  const uint32_t id = this->create(PORCH_LIGHT);
  ASSERT_NE(id, 0u);
  const size_t files_before = this->files().size();

  this->engine->loop_busy = true;
  // clang-format off
  const std::vector<std::pair<const char *, std::function<Reply()>>> calls = {
      {"list", [&] { return this->get("list"); }},
      {"get", [&] { return this->get("get?id=" + std::to_string(id)); }},
      {"export", [&] { return this->get("export"); }},
      {"save", [&] { return this->post("save", FAN); }},
      {"delete", [&] { return this->post("delete?id=" + std::to_string(id)); }},
  };
  // clang-format on
  for (const auto &call : calls) {
    Reply reply = call.second();
    EXPECT_EQ(reply.code, 503) << call.first << ": " << reply.body;
    EXPECT_EQ(reply.error(), "Device busy") << call.first;
  }
  this->engine->loop_busy = false;

  // Refused, not half done: the rule, its file and the rest of the list are as they were.
  EXPECT_EQ(this->engine->configs().size(), 1u);
  EXPECT_EQ(this->files().size(), files_before);
  EXPECT_EQ(this->get("get?id=" + std::to_string(id)).code, 200);
}

// A 404 raised inside the job is the job's answer, not the loop failing to run it.
TEST_F(Editor, AMissingRuleFoundOnTheLoopTaskKeepsItsOwnStatus) {
  Reply reply = this->get("get?id=404");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Automation not found");
  reply = this->post("delete?id=404");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Automation not found");
}

}  // namespace esphome::web_automation_editor::testing
