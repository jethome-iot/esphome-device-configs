// The virtual input routes over a real virtual_inputs pool, its file in a directory that stands in
// for the user partition. The timing and the boot are the virtual_inputs suite's.
#include "common.h"
#include <sys/stat.h>
#include <fstream>
#include <sstream>
#include "esphome/components/virtual_inputs/virtual_inputs.h"

namespace esphome::web_device_dashboard::testing {

static const char *const LIST = "/api/device/virtual-inputs";
static const char *const SAVE = "/api/device/virtual-inputs/save";
static const char *const DELETE = "/api/device/virtual-inputs/delete";
static const char *const FOLDER = ".storage/virtual_inputs";
static const char *const FILE_PATH = ".storage/virtual_inputs/config/virtual_inputs.json";

// The pool with what a case needs to put it in: a file that did not load, and a fresh boot.
class TestVirtualInputs : public virtual_inputs::VirtualInputs {
 public:
  void reset() {
    for (Slot *slot : this->slots_) {
      slot->config = virtual_inputs::InputConfig{};
      slot->source = nullptr;
      slot->line.reset();
      slot->entity.set_visible(false, 0);
      slot->entity.invalidate_state();
    }
    this->file_loaded_ = true;
    this->file_.set_slots(this->max_inputs_);
    this->cancel_timeout("ha_resync");
  }
  void fail_file() { this->file_loaded_ = false; }

 protected:
  void resync_home_assistant_() override {}
};

// One pool for the process: App keeps its entities, and the inputs keep callbacks into it.
struct Pool {
  dir_storage::DirStorage storage;
  config_json::ConfigJsonKeeper keeper;
  config_json::ConfigJsonKeeper failed;
  TestVirtualInputs inputs;
};

static Pool &pool() {
  static Pool *instance = [] {
    entities();
    mkdir(".storage", 0755);
    auto *p = new Pool();
    p->storage.set_base_path(FOLDER);
    p->storage.setup();
    p->keeper.set_storage(&p->storage);
    p->failed.set_storage(&p->storage);
    p->failed.mark_failed();
    p->inputs.set_max_inputs(3);
    p->inputs.set_keeper(&p->keeper);
    return p;
  }();
  return *instance;
}

static std::string read_file() {
  std::ifstream in(FILE_PATH);
  std::stringstream text;
  text << in.rdbuf();
  return text.str();
}

class VirtualInputRoutes : public Dashboard {
 protected:
  void SetUp() override {
    Dashboard::SetUp();
    Pool &p = pool();
    // The keepers above took the global over as they were made.
    config_json::global_config_json_keeper = &store().keeper;
    chmod((std::string(FOLDER) + "/config").c_str(), 0755);
    remove(FILE_PATH);
    entities().in1.publish_state(false);
    this->boot();
    this->dashboard->set_virtual_inputs(&p.inputs);
  }
  void TearDown() override {
    chmod((std::string(FOLDER) + "/config").c_str(), 0755);
    pool().inputs.set_rule_finder(nullptr);
    pool().inputs.reset();
    Dashboard::TearDown();
  }

  // A boot over what the file holds now.
  static void boot() {
    Pool &p = pool();
    p.inputs.reset();
    p.inputs.set_keeper(&p.keeper);
    p.inputs.setup();
  }

  Reply save(const std::string &body) { return this->post(SAVE, body); }
  Reply remove_slot(int slot) { return this->post(DELETE, R"({"slot":)" + std::to_string(slot) + "}"); }
};

TEST_F(VirtualInputRoutes, CapabilitiesNameThemOnlyWhenWired) {
  EXPECT_TRUE(this->get("/api/device/capabilities")["virtual_inputs"].as<bool>());
  this->dashboard->set_virtual_inputs(nullptr);
  EXPECT_TRUE(this->get("/api/device/capabilities")["virtual_inputs"].isNull());
}

TEST_F(VirtualInputRoutes, WithoutThemTheRoutesAreNotFound) {
  this->dashboard->set_virtual_inputs(nullptr);
  for (Reply reply : {this->get(LIST), this->save(R"({"source":"in_1"})"), this->remove_slot(1)}) {
    EXPECT_EQ(reply.code, 404);
    EXPECT_EQ(reply.error(), "No virtual inputs");
  }
}

TEST_F(VirtualInputRoutes, TheListStartsEmptyWithTheInputsASlotMayFollow) {
  store().bs.seed("in_1", false, "Hall");
  Reply reply = this->get(LIST);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply["max_inputs"].as<int>(), 3);
  EXPECT_EQ(reply["max_delay"].as<int>(), 86400);
  EXPECT_TRUE(reply["can_save"].as<bool>());
  EXPECT_EQ(reply["inputs"].as<JsonArray>().size(), 0u);
  ASSERT_EQ(reply["sources"].as<JsonArray>().size(), 1u);
  EXPECT_EQ(reply["sources"][0]["object_id"].as<std::string>(), "in_1");
  EXPECT_EQ(reply["sources"][0]["name"].as<std::string>(), "In 1");
  EXPECT_EQ(reply["sources"][0]["label"].as<std::string>(), "Hall");
  EXPECT_EQ(this->dashboard->jobs, 1);
}

TEST_F(VirtualInputRoutes, ASaveWithoutASlotTakesTheLowestFreeOne) {
  Reply created = this->save(R"({"source":"in_1","off_delay":600})");
  ASSERT_EQ(created.code, 200) << created.body;
  EXPECT_TRUE(created.success());
  EXPECT_EQ(created.message(), "Virtual input 1 created");
  JsonVariant input = created["input"];
  EXPECT_EQ(input["slot"].as<int>(), 1);
  EXPECT_EQ(input["object_id"].as<std::string>(), "virtual_input_1");
  EXPECT_EQ(input["name"].as<std::string>(), "Virtual input 1");
  EXPECT_EQ(input["label"].as<std::string>(), "");
  EXPECT_EQ(input["source"].as<std::string>(), "in_1");
  EXPECT_EQ(input["on_delay"].as<int>(), 0);
  EXPECT_EQ(input["off_delay"].as<int>(), 600);
  EXPECT_FALSE(input["state"].isNull());
  EXPECT_FALSE(input["state"].as<bool>());
  EXPECT_EQ(input["problem"].as<std::string>(), "");
  EXPECT_EQ(read_file(), R"({"version":1,"records":[{"slot":1,"source":"in_1","on_delay":0,"off_delay":600}]})");

  entities().in1.publish_state(true);
  Reply listed = this->get(LIST);
  ASSERT_EQ(listed["inputs"].as<JsonArray>().size(), 1u);
  EXPECT_TRUE(listed["inputs"][0]["state"].as<bool>());
  // A virtual input is no source.
  EXPECT_EQ(listed["sources"].as<JsonArray>().size(), 1u);

  // An explicit slot, then the one left over.
  EXPECT_EQ(this->save(R"({"slot":3,"source":"in_1"})").message(), "Virtual input 3 created");
  EXPECT_EQ(this->save(R"({"source":"in_1"})")["input"]["slot"].as<int>(), 2);
  Reply full = this->save(R"({"source":"in_1"})");
  EXPECT_EQ(full.code, 409);
  EXPECT_EQ(full.error(), "All 3 virtual inputs are in use");
  EXPECT_EQ(this->get(LIST)["inputs"].as<JsonArray>().size(), 3u);
}

TEST_F(VirtualInputRoutes, EachInputSaysWhatUsesItAndANewOneSkipsASlotARuleNames) {
  // The rules, as the firmware's engine names them; this suite has no relays' settings.
  pool().inputs.set_rule_finder([](uint32_t key) {
    return key == fnv1_hash("virtual_input_1")
               ? std::vector<std::string>{"Comfort while someone is home", "Hall light when someone is home"}
               : std::vector<std::string>{};
  });
  ASSERT_EQ(this->save(R"({"slot":2,"source":"in_1"})").code, 200);
  Reply skipped = this->save(R"({"source":"in_1"})");
  ASSERT_EQ(skipped.code, 200) << skipped.body;
  EXPECT_EQ(skipped["input"]["slot"].as<int>(), 3);
  EXPECT_EQ(skipped["input"]["used_by"]["rules"].as<JsonArray>().size(), 0u);
  // The one slot left is named: a new input takes it all the same.
  Reply named = this->save(R"({"source":"in_1"})");
  ASSERT_EQ(named.code, 200) << named.body;
  EXPECT_EQ(named.message(), "Virtual input 1 created");

  Reply listed = this->get(LIST);
  ASSERT_EQ(listed["inputs"].as<JsonArray>().size(), 3u);
  JsonVariant used = listed["inputs"][0]["used_by"];
  ASSERT_EQ(used["rules"].as<JsonArray>().size(), 2u);
  EXPECT_EQ(used["rules"][0].as<std::string>(), "Comfort while someone is home");
  EXPECT_EQ(used["rules"][1].as<std::string>(), "Hall light when someone is home");
  for (JsonVariant input : listed["inputs"].as<JsonArray>()) {
    ASSERT_TRUE(input["used_by"]["relays"].is<JsonArray>());
    EXPECT_EQ(input["used_by"]["relays"].as<JsonArray>().size(), 0u);
  }
  EXPECT_EQ(listed["inputs"][1]["used_by"]["rules"].as<JsonArray>().size(), 0u);
}

TEST_F(VirtualInputRoutes, AnEditKeepsTheDelaysItLeavesOut) {
  ASSERT_EQ(this->save(R"({"slot":2,"source":"in_1","on_delay":5,"off_delay":10})").code, 200);
  Reply edited = this->save(R"({"slot":2,"source":"in_1","off_delay":20})");
  ASSERT_EQ(edited.code, 200);
  EXPECT_EQ(edited.message(), "Virtual input 2 saved");
  EXPECT_EQ(edited["input"]["on_delay"].as<int>(), 5);
  EXPECT_EQ(edited["input"]["off_delay"].as<int>(), 20);
  // The same again is a success with nothing to change.
  EXPECT_EQ(this->save(R"({"slot":2,"source":"in_1"})").code, 200);
  EXPECT_EQ(read_file(), R"({"version":1,"records":[{"slot":2,"source":"in_1","on_delay":5,"off_delay":20}]})");
}

TEST_F(VirtualInputRoutes, ABadRequestIsRefusedWithNothingChanged) {
  ASSERT_EQ(this->save(R"({"source":"in_1"})").code, 200);
  const std::string before = read_file();
  const std::vector<std::pair<std::string, std::string>> refused = {
      {"", "Invalid JSON"},
      {"[1]", "Invalid JSON"},
      {"{}", "'source' must be the object id of an input"},
      {R"({"source":""})", "'source' must be the object id of an input"},
      {R"({"source":7})", "'source' must be the object id of an input"},
      {R"({"source":"in_1","slot":0})", "'slot' must be a number from 1 to 3"},
      {R"({"source":"in_1","slot":4})", "'slot' must be a number from 1 to 3"},
      {R"({"source":"in_1","slot":"1"})", "'slot' must be a number from 1 to 3"},
      {R"({"source":"in_1","slot":null})", "'slot' must be a number from 1 to 3"},
      {R"({"source":"in_1","on_delay":-1})", "'on_delay' must be a whole number of seconds from 0 to 86400"},
      {R"({"source":"in_1","on_delay":null})", "'on_delay' must be a whole number of seconds from 0 to 86400"},
      {R"({"source":"in_1","off_delay":86401})", "'off_delay' must be a whole number of seconds from 0 to 86400"},
      {R"({"source":"in_1","off_delay":1.5})", "'off_delay' must be a whole number of seconds from 0 to 86400"},
      {R"({"source":"in_1","off_delay":"600"})", "'off_delay' must be a whole number of seconds from 0 to 86400"},
      {R"({"slot":1,"source":"no_such_input"})", "'no_such_input' is not an input"},
      {R"({"slot":1,"source":"relay_1"})", "'relay_1' is not an input"},
      {R"({"slot":2,"source":"virtual_input_1"})",
       "'virtual_input_1' is a virtual input; the source must be a physical one"},
      {R"({"slot":2,"source":"virtual_input_3"})",
       "'virtual_input_3' is a virtual input; the source must be a physical one"},
  };
  for (const auto &[body, error] : refused) {
    Reply reply = this->save(body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), error) << body;
  }
  // Wrong before it is full: the request is checked first.
  ASSERT_EQ(this->save(R"({"source":"in_1"})").code, 200);
  ASSERT_EQ(this->save(R"({"source":"in_1"})").code, 200);
  EXPECT_EQ(this->save(R"({"source":"no_such_input"})").code, 400);
  EXPECT_EQ(this->call(HTTP_POST, SAVE, R"({"source":"in_1"})", 512, "text/plain").code, 415);
  Reply listed = this->get(LIST);
  EXPECT_EQ(listed["inputs"][0]["source"].as<std::string>(), "in_1");
}

TEST_F(VirtualInputRoutes, ADeleteFreesTheSlotAndRefusesAFreeOne) {
  ASSERT_EQ(this->save(R"({"slot":2,"source":"in_1"})").code, 200);
  Reply removed = this->remove_slot(2);
  ASSERT_EQ(removed.code, 200);
  EXPECT_TRUE(removed.success());
  EXPECT_EQ(removed.message(), "Virtual input 2 deleted");
  EXPECT_EQ(this->get(LIST)["inputs"].as<JsonArray>().size(), 0u);
  EXPECT_EQ(read_file(), R"({"version":1,"records":[]})");
  Reply again = this->remove_slot(2);
  EXPECT_EQ(again.code, 409);
  EXPECT_EQ(again.error(), "Virtual input 2 is free");
  for (const char *body : {"{}", R"({"slot":0})", R"({"slot":4})", R"({"slot":"2"})"}) {
    Reply reply = this->post(DELETE, body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "'slot' must be a number from 1 to 3") << body;
  }
  EXPECT_EQ(this->post(DELETE, "nope").error(), "Invalid JSON");
}

TEST_F(VirtualInputRoutes, StorageThatCannotKeepThemRefusesEveryWrite) {
  ASSERT_EQ(this->save(R"({"slot":1,"source":"in_1"})").code, 200);
  pool().inputs.set_keeper(&pool().failed);
  EXPECT_FALSE(this->get(LIST)["can_save"].as<bool>());
  for (Reply reply : {this->save(R"({"slot":1,"source":"in_1","off_delay":5})"), this->remove_slot(1)}) {
    EXPECT_EQ(reply.code, 503);
    EXPECT_EQ(reply.error(), "Virtual input storage unavailable");
  }
  pool().inputs.set_keeper(&pool().keeper);
  pool().inputs.fail_file();
  EXPECT_FALSE(this->get(LIST)["can_save"].as<bool>());
  for (Reply reply : {this->save(R"({"slot":1,"source":"in_1","off_delay":5})"), this->remove_slot(1)}) {
    EXPECT_EQ(reply.code, 503);
    EXPECT_EQ(reply.error(), "The virtual inputs file did not load; fix it and reboot");
  }
  EXPECT_EQ(this->dashboard->jobs, 3);  // the first save and the two reads; no refused write got that far
}

TEST_F(VirtualInputRoutes, AWriteThatFailsIs500AndChangesNothing) {
  ASSERT_EQ(this->save(R"({"slot":1,"source":"in_1"})").code, 200);
  chmod((std::string(FOLDER) + "/config").c_str(), 0555);
  for (Reply reply : {this->save(R"({"slot":2,"source":"in_1"})"), this->remove_slot(1)}) {
    EXPECT_EQ(reply.code, 500);
    EXPECT_EQ(reply.error(), "The virtual inputs were not written");
  }
  chmod((std::string(FOLDER) + "/config").c_str(), 0755);
  Reply listed = this->get(LIST);
  ASSERT_EQ(listed["inputs"].as<JsonArray>().size(), 1u);
  EXPECT_EQ(listed["inputs"][0]["slot"].as<int>(), 1);
}

TEST_F(VirtualInputRoutes, ALoopTaskThatDoesNotTakeTheJobIsBusy) {
  this->dashboard->loop_busy = true;
  for (Reply reply : {this->get(LIST), this->save(R"({"source":"in_1"})"), this->remove_slot(1)}) {
    EXPECT_EQ(reply.code, 503);
    EXPECT_EQ(reply.error(), "Device busy");
  }
  this->dashboard->loop_busy = false;
  EXPECT_EQ(this->get(LIST)["inputs"].as<JsonArray>().size(), 0u);
}

TEST_F(VirtualInputRoutes, TheListSaysWhyAnInputIsOffOrWaits) {
  {
    std::ofstream out(FILE_PATH);
    out << R"({"records":[{"slot":1,"source":"gone","off_delay":3}]})";
  }
  this->boot();
  Reply reply = this->get(LIST);
  ASSERT_EQ(reply["inputs"].as<JsonArray>().size(), 1u);
  EXPECT_EQ(reply["inputs"][0]["source"].as<std::string>(), "gone");
  EXPECT_EQ(reply["inputs"][0]["off_delay"].as<int>(), 3);
  EXPECT_FALSE(reply["inputs"][0]["state"].as<bool>());
  EXPECT_EQ(reply["inputs"][0]["problem"].as<std::string>(), "Source 'gone' not found");

  // A source that has not reported yet leaves the input without a state.
  entities().in1.invalidate_state();
  ASSERT_EQ(this->save(R"({"slot":2,"source":"in_1"})").code, 200);
  Reply listed = this->get(LIST);
  JsonVariant waiting = listed["inputs"][1];
  EXPECT_TRUE(waiting["state"].isNull());
  EXPECT_EQ(waiting["problem"].as<std::string>(), "Waiting for 'in_1' to report");
  entities().in1.publish_state(true);
  EXPECT_TRUE(this->get(LIST)["inputs"][1]["state"].as<bool>());
}

TEST_F(VirtualInputRoutes, ALabelIsTheBinarySensorRecordsAndTheIndexMarksTheInputVirtual) {
  ASSERT_EQ(this->save(R"({"source":"in_1"})").code, 200);
  store().bs.seed("virtual_input_1", false, "Someone home");
  EXPECT_EQ(this->get(LIST)["inputs"][0]["label"].as<std::string>(), "Someone home");

  Reply index = this->get("/api/device/entities");
  ASSERT_EQ(index.code, 200);
  JsonArray inputs = index["binary_sensor"].as<JsonArray>();
  ASSERT_EQ(inputs.size(), 2u);  // In 1 and the one virtual input in use
  EXPECT_EQ(inputs[0]["source_name"].as<std::string>(), "in_1");
  EXPECT_TRUE(inputs[0]["virtual"].isNull());
  EXPECT_EQ(inputs[1]["source_name"].as<std::string>(), "virtual_input_1");
  EXPECT_EQ(inputs[1]["name"].as<std::string>(), "Virtual input 1");
  EXPECT_EQ(inputs[1]["label"].as<std::string>(), "Someone home");
  EXPECT_TRUE(inputs[1]["virtual"].as<bool>());
  // Switches carry no such key.
  for (JsonVariant relay : index["switch"].as<JsonArray>())
    EXPECT_TRUE(relay["virtual"].isNull());
}

}  // namespace esphome::web_device_dashboard::testing
