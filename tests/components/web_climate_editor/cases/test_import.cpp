#include "common.h"

// import: a thermostat brought back under the id its body names, with its presets' keys and its
// active preset, answered as save answers. The requests the client mock answers too are in
// ../contract.json; here is what only the device shows: the files, the entities and the relays.
namespace esphome::web_climate_editor::testing {
namespace {

// A backup's document: an id that is not its name's slug, keys no create would make, and the
// preset picked last with a target set by hand since.
const char *const LOUNGE =
    R"({"version":2,"id":"lounge","name":"Living Room","kind":"pid","sensor_id":"room",)"
    R"("heat":{"relay_id":"relay_1"},"mode":"heat","setpoint":20.5,"presets":[)"
    R"({"key":"day-time","name":"Comfort","setpoint":22,"mode":"keep"},)"
    R"({"key":"night-2","name":"Night","setpoint":19,"mode":"heat"}],"active_preset":"night-2"})";

// @p json with the first @p from replaced by @p to.
std::string replaced(const std::string &json, const std::string &from, const std::string &to) {
  std::string out = json;
  out.replace(out.find(from), from.size(), to);
  return out;
}

class Import : public Editor {
 protected:
  Reply import(const std::string &body) { return this->post("import", body); }

  // Files written by hand, read by a boot.
  void boot_with(const std::vector<std::string> &docs) {
    mkdir(this->folder().c_str(), 0755);
    for (const std::string &doc : docs) {
      JsonDocument parsed;
      ASSERT_EQ(deserializeJson(parsed, doc), DeserializationError::Ok) << doc;
      std::ofstream(this->folder() + "/" + parsed["id"].as<std::string>() + ".json") << doc;
    }
    hub().reset();
    hub().setup();
  }
};

}  // namespace

TEST_F(Import, CreatesTheThermostatUnderItsIdWithItsPresetsKeysAndActivePreset) {
  Reply reply = this->import(LOUNGE);
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat created","id":"lounge"})");
  EXPECT_EQ(this->files(), std::vector<std::string>{"lounge.json"});
  EXPECT_TRUE(hub().is_running("lounge"));
  EXPECT_EQ(hub().claimed_by("relay_1"), "lounge");

  Reply got = this->get("get?id=lounge");
  ASSERT_EQ(got.code, 200) << got.body;
  EXPECT_EQ(got["presets"][0]["key"].as<std::string>(), "day-time");
  EXPECT_EQ(got["presets"][1]["key"].as<std::string>(), "night-2");
  EXPECT_EQ(got["active_preset"].as<std::string>(), "night-2");
  EXPECT_FLOAT_EQ(got["setpoint"].as<float>(), 20.5f);
  EXPECT_EQ(this->get("list")["controllers"][0]["active_preset_name"].as<std::string>(), "Night");
}

// The restore's round trip: what get answered comes back as it was, under its id.
TEST_F(Import, WhatGetAnswersComesBackAsItWas) {
  ASSERT_EQ(this->post("save", with(LIVING_ROOM, R"("presets":[{"name":"Eco","setpoint":18}])")).code, 200);
  ASSERT_EQ(this->post("preset?id=living-room&key=eco").code, 200);
  ASSERT_EQ(this->post("setpoint?id=living-room&value=19").code, 200);
  const std::string before = this->get("get?id=living-room").body;
  ASSERT_EQ(this->post("delete?id=living-room").code, 200);

  Reply reply = this->import(before);
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat created","id":"living-room"})");
  EXPECT_EQ(this->get("get?id=living-room").body, before);
  EXPECT_TRUE(hub().is_running("living-room"));
}

// In place: the entity and the relay it holds stay; the keys and the active preset are the body's.
TEST_F(Import, ReplacesTheThermostatWithItsIdInPlace) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  const climate_hub::ControllerRuntime *runtime = hub().runtime("living-room");

  Reply reply = this->import(replaced(LOUNGE, "lounge", "living-room"));
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat replaced","id":"living-room"})");
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
  EXPECT_EQ(hub().runtime("living-room"), runtime);
  EXPECT_EQ(hub().claimed_by("relay_1"), "living-room");
  Reply got = this->get("get?id=living-room");
  EXPECT_EQ(got["presets"][1]["key"].as<std::string>(), "night-2") << "a save would have made it night";
  EXPECT_EQ(got["active_preset"].as<std::string>(), "night-2");
}

TEST_F(Import, RefusesWhatItCannotRead) {
  struct Case {
    std::string body;
    const char *content_type;
    const char *error;
  };
  for (const Case &c : {
           Case{"", "application/json", "Empty request body"},
           Case{LOUNGE, "application/x-www-form-urlencoded", "Empty request body"},
           Case{"garbage", "application/json", "JSON parse error: InvalidInput"},
           Case{"[]", "application/json", "document is not an object"},
           // The id first, as the boot reads a file.
           Case{LIVING_ROOM, "application/json", "id is required"},
           Case{R"({"id":"","name":"A"})", "application/json", "id is required"},
           Case{R"({"id":5,"name":"A"})", "application/json", "id is required"},
           Case{replaced(LOUNGE, "lounge", "Lounge"), "application/json",
                "id must be a slug: lowercase letters, digits and single dashes"},
           Case{replaced(LOUNGE, "lounge", "../lounge"), "application/json",
                "id must be a slug: lowercase letters, digits and single dashes"},
           Case{R"({"id":"lounge","name":"Lounge"})", "application/json", "sensor_id is required"},
           Case{replaced(LOUNGE, "night-2\",\"name", "Night\",\"name"), "application/json",
                "Preset 2: key must be a slug: lowercase letters, digits and single dashes"},
           Case{replaced(LOUNGE, "Living Room", "Up/down"), "application/json", "Name cannot contain '/'"},
           // The editor opens a blank form there, and a boot refuses such a file.
           Case{replaced(LOUNGE, "lounge", "new"), "application/json", "id 'new' is reserved"},
           Case{R"({"id":"new","name":"Lounge"})", "application/json", "sensor_id is required"},
       }) {
    Reply reply = this->call(HTTP_POST, "import", c.body, c.content_type);
    EXPECT_EQ(reply.code, 400) << c.body;
    EXPECT_EQ(reply.error(), c.error) << c.body;
  }
  EXPECT_TRUE(this->files().empty());
  EXPECT_EQ(hub().store().size(), 0u);
}

TEST_F(Import, RefusesABodyOverEightKiB) {
  const std::string lounge = LOUNGE;
  Reply over = this->import(lounge + std::string(8193 - lounge.size(), ' '));
  EXPECT_EQ(over.code, 413);
  EXPECT_EQ(over.error(), "Request body over 8 KiB");
  Reply fits = this->import(lounge + std::string(8192 - lounge.size(), ' '));
  EXPECT_EQ(fits.code, 200) << fits.body;
}

// The refusals a save gives, in a save's order: the limit, the name, then the sensor's unit and
// the relays of an enabled one.
TEST_F(Import, AnswersAsASaveDoes) {
  ASSERT_EQ(this->create(FLOOR), "floor");

  Reply taken = this->import(replaced(LOUNGE, "Living Room", "floor"));
  EXPECT_EQ(taken.code, 409);
  EXPECT_EQ(taken.error(), "\"floor\" is already used by another thermostat");
  EXPECT_EQ(this->import(replaced(LOUNGE, "Living Room", "Hall")).code, 409) << "a YAML climate's name";

  Reply unit = this->import(replaced(LOUNGE, "\"room\"", "\"uptime\""));
  EXPECT_EQ(unit.code, 400);
  EXPECT_EQ(unit.error(), "\"Uptime\" reports s, not °C");

  Reply held = this->import(replaced(LOUNGE, "relay_1", "relay_2"));
  EXPECT_EQ(held.code, 409);
  EXPECT_EQ(held.error(), "\"Relay 2\" is already driven by \"Floor\"");

  // An enabled one that waits reserves its relay.
  Reply waits = this->import(replaced(replaced(LOUNGE, "\"room\"", "\"attic\""), "Living Room", "Attic"));
  ASSERT_EQ(waits.code, 200) << waits.body;
  EXPECT_EQ(waits.body, R"({"success":true,"message":"Thermostat created; not started: sensor 'attic' not found",)"
                        R"("id":"lounge","warning":"not started: sensor 'attic' not found"})");
  Reply reserved = this->import(replaced(LOUNGE, "lounge", "den"));
  EXPECT_EQ(reserved.code, 409);
  EXPECT_EQ(reserved.error(), "\"Relay 1\" is reserved by \"Attic\", which is enabled and waits to start");

  Reply disabled = this->import(with(replaced(LOUNGE, "lounge", "den").c_str(), R"("enabled":false)"));
  ASSERT_EQ(disabled.code, 200) << disabled.body;
  Reply limit = this->import(replaced(replaced(LOUNGE, "lounge", "porch"), "Living Room", "floor"));
  EXPECT_EQ(limit.code, 507) << "the limit comes before the name";
  EXPECT_EQ(limit.error(), "This device allows 3 thermostats; delete one to add another");
  EXPECT_EQ(this->files().size(), 3u);
}

// A replaced thermostat that lets a relay go starts the one that waited for it, before its own
// warning, as a save does.
TEST_F(Import, AReplacementThatFreesARelayNamesWhoStarted) {
  this->boot_with({R"({"version":1,"id":"summer","name":"Summer","sensor_id":"room","heat":{"relay_id":"relay_1"}})",
                   R"({"version":1,"id":"winter","name":"Winter","sensor_id":"room","heat":{"relay_id":"relay_1"}})"});
  ASSERT_FALSE(hub().is_running("winter"));
  Reply reply = this->import(R"({"id":"summer","name":"Summer","sensor_id":"attic","heat":{"relay_id":"relay_2"}})");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body,
            R"({"success":true,"message":"Thermostat replaced; \"Winter\" started; not started: )"
            R"(sensor 'attic' not found","id":"summer","warning":"not started: sensor 'attic' not found"})");
  EXPECT_EQ(hub().claimed_by("relay_1"), "winter");
}

// A file the boot refused holds its id: the import names it rather than write over it.
TEST_F(Import, RefusesAnIdAFileTheBootDidNotLoadHolds) {
  std::ofstream(this->folder() + "/lounge.json") << "left alone";
  this->boot_with({});
  ASSERT_EQ(hub().store().size(), 0u);
  Reply reply = this->import(LOUNGE);
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "The id \"lounge\" is taken by a file in the thermostat folder that was not loaded");
  EXPECT_EQ(this->file("lounge.json"), "left alone");
}

// As for a save: a newer firmware's file is never written over.
TEST_F(Import, RefusesToReplaceAThermostatANewerFirmwareWrote) {
  const std::string text = R"({"version":)" + std::to_string(climate_hub::CONFIG_VERSION + 1) +
                           R"(,"id":"lounge","name":"Lounge","sensor_id":"room",)"
                           R"("heat":{"relay_id":"relay_1"},"future":1})";
  this->boot_with({text});
  Reply reply = this->import(LOUNGE);
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "A newer firmware wrote this thermostat; update the firmware to change it");
  EXPECT_EQ(this->file("lounge.json"), text);
}

// A backup a newer firmware made comes back in this firmware's format, as a save writes it.
TEST_F(Import, WritesThisFirmwaresVersion) {
  const std::string version = "\"version\":";
  Reply reply =
      this->import(replaced(LOUNGE, version + "2", version + std::to_string(climate_hub::CONFIG_VERSION + 1)));
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(this->get("get?id=lounge")["version"].as<int>(), climate_hub::CONFIG_VERSION);
  EXPECT_NE(this->file("lounge.json").find(version + std::to_string(climate_hub::CONFIG_VERSION)), std::string::npos);
}

TEST_F(Import, AFileThatCannotBeWrittenChangesNothing) {
  storage().set_base_path("/proc/definitely-not-writable");
  Reply reply = this->import(LOUNGE);
  storage().set_base_path(this->base_path);
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "The thermostat's file could not be written");
  EXPECT_EQ(hub().store().size(), 0u);

  hub().max_file_bytes = 100;
  reply = this->import(LOUNGE);
  EXPECT_EQ(reply.code, 413);
  EXPECT_EQ(reply.error(), "The thermostat's file would be over 8 KiB");
  EXPECT_TRUE(this->files().empty());
}

}  // namespace esphome::web_climate_editor::testing
