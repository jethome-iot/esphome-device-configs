#include <cctype>
#include <initializer_list>
#include "common.h"

// What only the device shows, beside contract.json: the relays and the thermostats a request
// moves, the files it writes, and the answers to states the API alone cannot reach: two enabled
// schedules on one target written by hand, a relay a running thermostat holds, a file the
// partition will not let go of.
namespace esphome::web_schedule_editor::testing {

class Api : public Editor {
 protected:
  // A schedule as a file holds it, on at 08:00 on Monday, on `relay`.
  static std::string file_doc(const char *id, const char *name, const char *relay = "relay_3") {
    return std::string(R"({"version":1,"id":")") + id + R"(","name":")" + name +
           R"(","enabled":true,"modes":[{"id":"on","name":"On","actions":[{"type":"relay","id":")" + relay +
           R"(","value":{"type":"on"}}]}],"week":{"mon":[{"at":"08:00","mode":"on"}]}})";
  }
  void write(const std::string &name, const std::string &text) {
    mkdir(this->folder().c_str(), 0755);
    std::ofstream(this->folder() + "/" + name) << text;
  }
  // Files written by hand, two or more enabled on relay_3, and a boot over them: the first by id
  // drives it, the others wait.
  void boot_with_conflict(std::initializer_list<const char *> names) {
    for (const char *name : names) {
      std::string id = name;
      std::transform(id.begin(), id.end(), id.begin(), ::tolower);
      this->write(id + ".json", file_doc(id.c_str(), name));
    }
    this->reboot();
  }
  std::vector<std::string> ids(JsonVariant list) {
    std::vector<std::string> out;
    for (JsonVariant id : list.as<JsonArray>())
      out.push_back(id.as<std::string>());
    return out;
  }
};

TEST_F(Api, ASaveOfAnEnabledScheduleDrivesItsRelayAtOnce) {
  this->set_clock(at_local(2, 20, 0));
  ASSERT_EQ(this->create(porch()), "porch");
  EXPECT_EQ(this->e.relay3.writes, std::vector<bool>{true}) << "on since 19:00";
  // A rename is a Save too: the list of the mode in force runs again.
  EXPECT_EQ(this->post("save", with(porch("Front porch"), R"("id":"porch")")).code, 200);
  EXPECT_EQ(this->e.relay3.writes, (std::vector<bool>{true, true}));
  // A disabled one leaves it as it is.
  EXPECT_EQ(this->post("save", with(porch("Front porch", "relay_3", false), R"("id":"porch")")).code, 200);
  EXPECT_EQ(this->e.relay3.writes.size(), 2u);
}

TEST_F(Api, GetAnswersTheFileAsItIs) {
  ASSERT_EQ(this->create(porch()), "porch");
  Reply reply = this->get("get?id=porch");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, this->file("porch.json"));
}

TEST_F(Api, DisablingTheScheduleThatDrivesATargetStartsTheNextAndNamesIt) {
  this->boot_with_conflict({"A", "B"});
  Reply list = this->get("list");
  ASSERT_EQ(list["schedules"][1]["status"]["state"].as<std::string>(), "conflict");
  EXPECT_EQ(list["schedules"][1]["status"]["reason"].as<std::string>(), "Schedule \"A\" drives Relay 3");

  Reply reply = this->post("enable?id=a&value=false");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Schedule disabled; \"B\" started");
  EXPECT_EQ(this->ids(reply["started"]), std::vector<std::string>{"b"});
  EXPECT_TRUE(reply["stopped"].isUnbound());
  EXPECT_EQ(this->get("list")["schedules"][1]["status"]["state"].as<std::string>(), "running");
}

TEST_F(Api, DeletingTheScheduleThatDrivesATargetStartsTheNext) {
  this->boot_with_conflict({"A", "B"});
  Reply reply = this->post("delete?id=a");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Schedule deleted; \"B\" started");
  EXPECT_EQ(this->ids(reply["started"]), std::vector<std::string>{"b"});
  EXPECT_TRUE(reply["persisted"].as<bool>());
  EXPECT_EQ(this->files(), std::vector<std::string>{"b.json"});
}

// The message names the schedules that started first, then why the Save does not run itself.
TEST_F(Api, ASaveThatLeavesATargetStartsTheNextBeforeItsOwnWarning) {
  this->boot_with_conflict({"A", "B"});
  Reply reply = this->post("save", with(porch("A", "relay_7"), R"("id":"a")"));
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(),
            "Schedule updated; \"B\" started; skips mode \"On\", action 1: relay \"relay_7\" is not on this device");
  EXPECT_EQ(this->ids(reply["started"]), std::vector<std::string>{"b"});
  EXPECT_EQ(reply["warning"].as<std::string>(), "skips mode \"On\", action 1: relay \"relay_7\" is not on this device");
}

TEST_F(Api, ATakeOverNamesEveryScheduleItStopped) {
  this->boot_with_conflict({"A", "B", "C"});
  Reply refused = this->post("enable?id=c&value=true");
  EXPECT_EQ(refused.code, 409);
  EXPECT_EQ(refused.error(), "\"A\" already drives Relay 3; switch it off first or take over");

  Reply reply = this->post("enable?id=c&value=true&take_over=true");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Schedule enabled; \"A\" and \"B\" stopped");
  EXPECT_EQ(this->ids(reply["stopped"]), (std::vector<std::string>{"a", "b"}));
  EXPECT_TRUE(reply["started"].isUnbound());
  EXPECT_TRUE(reply["persisted"].as<bool>());
  // Stored disabled, so the next boot agrees.
  this->reboot();
  Reply list = this->get("list");
  EXPECT_FALSE(list["schedules"][0]["enabled"].as<bool>());
  EXPECT_FALSE(list["schedules"][1]["enabled"].as<bool>());
  EXPECT_EQ(list["schedules"][2]["status"]["state"].as<std::string>(), "running");
}

// A relay a running thermostat drives: the list says who, for each action on it, /targets too,
// and the relay takes the mode in force once the thermostat lets go.
TEST_F(Api, ARelayARunningThermostatDrivesHoldsItsActionsBack) {
  climate_hub::ClimateConfig floor;
  floor.name = "Floor";
  floor.sensor_id = "room";
  floor.heat.relay_id = "relay_2";
  floor.mode = climate_hub::HubMode::HEAT;
  const climate_hub::Result made = hub().create(floor);
  ASSERT_TRUE(made.ok) << made.error;
  this->set_clock(at_local(2, 20, 0));
  this->e.relay2.writes.clear();
  ASSERT_EQ(this->create(porch("Porch", "relay_2")), "porch");
  EXPECT_TRUE(this->e.relay2.writes.empty());

  Reply list = this->get("list");
  JsonObject status = list["schedules"][0]["status"];
  EXPECT_EQ(status["state"].as<std::string>(), "running");
  EXPECT_EQ(status["reason"].as<std::string>(), "");
  ASSERT_EQ(status["issues"].size(), 2u);
  for (JsonObject issue : status["issues"].as<JsonArray>()) {
    EXPECT_EQ(issue["kind"].as<std::string>(), "held");
    EXPECT_EQ(issue["held_by"].as<std::string>(), "Floor");
    EXPECT_EQ(issue["text"].as<std::string>(), "Thermostat \"Floor\" drives Relay 2");
    EXPECT_EQ(issue["action"].as<int>(), 0);
  }
  EXPECT_EQ(status["issues"][0]["mode"].as<std::string>(), "on");
  EXPECT_EQ(status["issues"][1]["mode"].as<std::string>(), "off");
  Reply targets = this->get("targets");
  for (JsonObject relay : targets["relays"].as<JsonArray>()) {
    if (relay["object_id"] == "relay_2") {
      EXPECT_EQ(relay["held_by"].as<std::string>(), "Floor");
      EXPECT_EQ(relay["scheduled_by"].as<std::string>(), "porch");
    }
  }

  ASSERT_TRUE(hub().remove(made.id).ok);
  engine().run_later();
  EXPECT_TRUE(this->e.relay2.state) << "on since 19:00, once the thermostat let go";
  EXPECT_EQ(this->get("list")["schedules"][0]["status"]["issues"].size(), 0u);
}

// The thermostats a schedule may drive, with the range a target is held in and their presets;
// an internal switch is no one's to schedule.
TEST_F(Api, TargetsListsWhatASchedulePicksFrom) {
  Reply reply = this->get("targets");
  EXPECT_EQ(reply.code, 200);
  std::vector<std::string> relays;
  for (JsonObject relay : reply["relays"].as<JsonArray>())
    relays.push_back(relay["object_id"].as<std::string>());
  EXPECT_EQ(relays, (std::vector<std::string>{"relay_1", "relay_2", "relay_3", "boiler", "pump"}));
  JsonArray thermostats = reply["thermostats"];
  ASSERT_EQ(thermostats.size(), 2u);
  EXPECT_EQ(thermostats[0]["id"].as<std::string>(), "hall");
  EXPECT_EQ(thermostats[0]["mode"].as<std::string>(), "off");
  EXPECT_EQ(thermostats[0]["presets"].size(), 0u);
  EXPECT_EQ(thermostats[1]["presets"][1]["key"].as<std::string>(), "eco");
  EXPECT_FLOAT_EQ(thermostats[1]["presets"][1]["setpoint"].as<float>(), 19.f);
}

TEST_F(Api, AThermostatActionGoesThroughTheHub) {
  this->set_clock(at_local(2, 18, 0));
  ASSERT_EQ(this->create(living_room_week()), "living-room-week");
  EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, 21.5f);
  Reply list = this->get("list");
  EXPECT_EQ(list["schedules"][0]["now"]["mode"].as<std::string>(), "warm");
  EXPECT_EQ(list["schedules"][0]["now"]["at"].as<std::string>(), "17:00");
  EXPECT_EQ(list["schedules"][0]["next"]["day"].as<std::string>(), "wed");
  EXPECT_EQ(list["schedules"][0]["next"]["at"].as<std::string>(), "08:30");
  EXPECT_EQ(list["schedules"][0]["next"]["mode"].as<std::string>(), "eco");
}

// A list with a wait goes on while the device answers: the next request finds it further on.
TEST_F(Api, AListWithAWaitGoesOnAfterTheAnswer) {
  this->set_clock(at_local(2, 20, 0));
  ASSERT_EQ(this->create(R"({"name":"Pump","modes":[{"id":"run","name":"Run","actions":[)"
                         R"({"type":"relay","id":"relay_1","value":{"type":"on"}},{"type":"wait","seconds":1},)"
                         R"({"type":"relay","id":"relay_2","value":{"type":"on"}}]}],)"
                         R"("week":{"wed":[{"at":"19:00","mode":"run"}]}})"),
            "pump");
  EXPECT_TRUE(this->e.relay1.state);
  EXPECT_FALSE(this->e.relay2.state) << "the wait is the scheduler's";
}

TEST_F(Api, AFlagThatDidNotReachFlashSaysSo) {
  ASSERT_EQ(this->create(porch()), "porch");
  storage().path = "/proc/definitely-not-writable";
  Reply reply = this->post("enable?id=porch&value=false");
  storage().path = this->base_path;
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Schedule disabled");
  EXPECT_FALSE(reply["persisted"].as<bool>());
  EXPECT_FALSE(this->get("list")["schedules"][0]["enabled"].as<bool>()) << "live all the same";
}

// Neither unlinked nor emptied: the partition refuses both, so the next boot brings it back.
TEST_F(Api, ADeleteWhoseFileStaysSaysItComesBack) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes a read-only file";
  ASSERT_EQ(this->create(porch()), "porch");
  engine().refuse_remove = true;
  chmod((this->folder() + "/porch.json").c_str(), 0444);
  Reply reply = this->post("delete?id=porch");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Schedule deleted; its file could not be removed, so it comes back at the next boot");
  EXPECT_FALSE(reply["persisted"].as<bool>());
  EXPECT_EQ(this->get("list")["count"].as<int>(), 0);
  engine().refuse_remove = false;
  this->reboot();
  EXPECT_EQ(this->get("list")["count"].as<int>(), 1);
}

// A file the boot refused still holds its id: an import never writes over it.
TEST_F(Api, AnImportOntoAFileTheBootRefusedIs409) {
  this->write("porch.json", "{ not json");
  this->reboot();
  EXPECT_EQ(this->get("list")["count"].as<int>(), 0);
  Reply reply = this->post("import", with(porch(), R"("id":"porch")"));
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "The id \"porch\" is taken by a file in the schedule folder that was not loaded");
  EXPECT_EQ(this->file("porch.json"), "{ not json");
  // A Save makes its id from the name and steps past it.
  EXPECT_EQ(this->create(porch()), "porch-2");
}

TEST_F(Api, SchemaIsBuildTimeDataAndNeverWaitsForTheLoop) {
  engine().loop_busy = true;
  Reply reply = this->get("schema");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, R"({"version":1,"max_schedules":3,"max_points_per_day":12,"name_max_length":48,)"
                        R"("max_modes":8,"max_actions":12,"mode_name_max_length":24,"mode_id_max_length":24,)"
                        R"("min_wait_seconds":1,"max_wait_seconds":86400,)"
                        R"("days":["mon","tue","wed","thu","fri","sat","sun"],)"
                        R"("colors":["red","orange","amber","yellow","lime","green","teal","cyan","blue","indigo",)"
                        R"("violet","pink","grey"],)"
                        R"("values":{"relay":["on","off"],"thermostat":["on","off","preset","target"]}})");
}

}  // namespace esphome::web_schedule_editor::testing
