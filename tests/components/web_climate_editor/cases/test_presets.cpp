#include "common.h"

// The presets over HTTP: what get and save carry, the refusals in the order client/mock gives
// them, the active preset in list and status, what schema offers, and a file a newer firmware
// wrote, which a Save may not overwrite.
namespace esphome::web_climate_editor::testing {
namespace {

// Heats on relay_1 and cools on relay_2: two built-in presets, one by a name in lower case, and
// a custom one whose name keeps its inner spaces.
const char *const STUDIO = R"({"name":"Studio","kind":"bang_bang","sensor_id":"room","heat":{"relay_id":"relay_1"},)"
                           R"("cool":{"relay_id":"relay_2"},"mode":"heat","setpoint":21,"presets":[)"
                           R"({"name":"Eco","setpoint":18},{"name":" away ","setpoint":12,"mode":"off"},)"
                           R"({"name":"Night  Time","setpoint":19.5,"mode":"heat_cool"}]})";

const char *const STUDIO_PRESETS =
    R"([{"key":"eco","name":"Eco","setpoint":18,"mode":"keep"},{"key":"away","name":"away","setpoint":12,"mode":"off"},)"
    R"({"key":"night-time","name":"Night  Time","setpoint":19.5,"mode":"heat_cool"}])";

std::string text_of(JsonVariant value) {
  std::string out;
  serializeJson(value, out);
  return out;
}

// The thermostat's document as get answers it, ready to be changed and saved back.
JsonDocument document(Reply &got) {
  JsonDocument doc;
  doc.set(got.json);
  return doc;
}

std::string body_of(const JsonDocument &doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

climate::Climate *entity_named(const char *name) {
  for (climate::Climate *entity : App.get_climates()) {
    if (entity->get_name() == name)
      return entity;
  }
  return nullptr;
}

}  // namespace

TEST_F(Editor, GetAndSaveCarryThePresets) {
  ASSERT_EQ(this->create(STUDIO), "studio");
  Reply got = this->get("get?id=studio");
  ASSERT_EQ(got.code, 200) << got.body;
  EXPECT_EQ(text_of(got["presets"]), STUDIO_PRESETS);
  EXPECT_EQ(got["active_preset"].as<std::string>(), "");
  // After the target, where the file has them.
  EXPECT_NE(got.body.find(R"("setpoint":21,"presets":[)"), std::string::npos) << got.body;
  EXPECT_NE(got.body.find(R"(],"active_preset":""})"), std::string::npos) << got.body;

  // What get answers, save takes back as it is.
  Reply saved = this->post("save", got.body);
  ASSERT_EQ(saved.code, 200) << saved.body;
  EXPECT_EQ(text_of(this->get("get?id=studio")["presets"]), STUDIO_PRESETS);
}

// The keys are the device's to give, as the id is: made from the names, never from the body.
TEST_F(Editor, ACreateMakesTheKeysAndPicksNoPreset) {
  std::string body = with(LIVING_ROOM, R"("active_preset":"mine","presets":[)"
                                       R"({"key":"mine","name":"Day!","setpoint":20},)"
                                       R"({"name":"Day?","setpoint":21},{"name":"!!!","setpoint":17}])");
  ASSERT_EQ(this->create(body.c_str()), "living-room");
  Reply got = this->get("get?id=living-room");
  EXPECT_EQ(text_of(got["presets"]), R"([{"key":"day","name":"Day!","setpoint":20,"mode":"keep"},)"
                                     R"({"key":"day-2","name":"Day?","setpoint":21,"mode":"keep"},)"
                                     R"({"key":"preset","name":"!!!","setpoint":17,"mode":"keep"}])");
  EXPECT_EQ(got["active_preset"].as<std::string>(), "");
  EXPECT_FLOAT_EQ(got["setpoint"].as<float>(), 22.f);
}

// A rename keeps the key, so a rule naming it still finds it; a key the thermostat never gave
// out is made again from the name, and the body's active_preset is not the thermostat's state.
TEST_F(Editor, ASaveKeepsTheKeysItGaveOutAndTheActivePreset) {
  ASSERT_EQ(this->create(STUDIO), "studio");
  ASSERT_TRUE(hub().apply_preset("studio", "eco").ok);

  Reply got = this->get("get?id=studio");
  JsonDocument doc = document(got);
  doc["presets"][0]["name"] = "Saver";
  JsonObject added = doc["presets"].add<JsonObject>();
  added["key"] = "made-up";
  added["name"] = "Comfort";
  added["setpoint"] = 22;
  doc["active_preset"] = "away";
  Reply saved = this->post("save", body_of(doc));
  ASSERT_EQ(saved.code, 200) << saved.body;
  EXPECT_EQ(saved.body, R"({"success":true,"message":"Thermostat updated","id":"studio"})");

  got = this->get("get?id=studio");
  EXPECT_EQ(got["presets"][0]["key"].as<std::string>(), "eco");
  EXPECT_EQ(got["presets"][0]["name"].as<std::string>(), "Saver");
  EXPECT_EQ(got["presets"][3]["key"].as<std::string>(), "comfort");
  EXPECT_EQ(got["active_preset"].as<std::string>(), "eco");
  Reply list = this->get("list");
  JsonObject row = list["controllers"][0];
  EXPECT_EQ(row["active_preset"].as<std::string>(), "eco");
  EXPECT_EQ(row["active_preset_name"].as<std::string>(), "Saver");
}

// The active preset's new values are the thermostat's at once, over what the body says; new values
// for another preset leave the target and the mode the body sends.
TEST_F(Editor, NewValuesForTheActivePresetApplyAtOnce) {
  ASSERT_EQ(this->create(STUDIO), "studio");
  ASSERT_TRUE(hub().apply_preset("studio", "eco").ok);

  Reply got = this->get("get?id=studio");
  JsonDocument doc = document(got);
  doc["presets"][0]["setpoint"] = 17;
  doc["presets"][0]["mode"] = "cool";
  doc["setpoint"] = 25;
  ASSERT_EQ(this->post("save", body_of(doc)).code, 200);
  got = this->get("get?id=studio");
  EXPECT_FLOAT_EQ(got["setpoint"].as<float>(), 17.f);
  EXPECT_EQ(got["mode"].as<std::string>(), "cool");
  EXPECT_EQ(got["active_preset"].as<std::string>(), "eco");
  EXPECT_FLOAT_EQ(this->get("status?id=studio")["controllers"][0]["setpoint"].as<float>(), 17.f);

  doc = document(got);
  doc["presets"][1]["setpoint"] = 10;
  doc["setpoint"] = 20;
  doc["mode"] = "heat";
  ASSERT_EQ(this->post("save", body_of(doc)).code, 200);
  got = this->get("get?id=studio");
  EXPECT_FLOAT_EQ(got["setpoint"].as<float>(), 20.f);
  EXPECT_EQ(got["mode"].as<std::string>(), "heat");
  EXPECT_EQ(got["active_preset"].as<std::string>(), "eco") << "a target set by hand keeps the label";
}

// Missing, or null, is no presets: a Save that leaves them out removes them, the active one too.
TEST_F(Editor, ASaveWithoutPresetsRemovesThem) {
  ASSERT_EQ(this->create(STUDIO), "studio");
  ASSERT_TRUE(hub().apply_preset("studio", "away").ok);
  const std::string studio = STUDIO;
  const std::string bare = studio.substr(0, studio.find(R"(,"presets")")) + "}";
  Reply saved = this->post("save", with(bare.c_str(), R"("id":"studio")"));
  ASSERT_EQ(saved.code, 200) << saved.body;
  Reply got = this->get("get?id=studio");
  EXPECT_EQ(text_of(got["presets"]), "[]");
  EXPECT_EQ(got["active_preset"].as<std::string>(), "");
  EXPECT_EQ(this->get("status?id=studio")["controllers"][0]["active_preset"].as<std::string>(), "");

  ASSERT_EQ(this->post("save", with(LIVING_ROOM, R"("enabled":false,"presets":null)")).code, 200);
  EXPECT_EQ(text_of(this->get("get?id=living-room")["presets"]), "[]");
}

// Each rule a preset can break, in the device's words, the row named from 1. The mock gives the
// same sentence for each body.
TEST_F(Editor, SaveRefusesABrokenPreset) {
  const char *cellar = R"({"name":"Cellar","sensor_id":"floor","cool":{"relay_id":"relay_2"},"mode":"cool"})";
  std::string nine = "[";
  for (int i = 1; i <= 9; i++)
    nine += std::string(i > 1 ? "," : "") + R"({"name":"P)" + std::to_string(i) + R"(","setpoint":20})";
  nine += "]";
  struct Case {
    const char *body;
    std::string presets;
    const char *error;
  };
  for (const Case &c : {
           Case{LIVING_ROOM, "{}", "presets must be a list"},
           Case{LIVING_ROOM, R"("eco")", "presets must be a list"},
           Case{LIVING_ROOM, nine, "A thermostat has at most 8 presets"},
           Case{LIVING_ROOM, R"([{"name":"Eco","setpoint":18},5])", "Preset 2 must be an object"},
           Case{LIVING_ROOM, R"([[{"name":"Eco","setpoint":18}]])", "Preset 1 must be an object"},
           Case{LIVING_ROOM, R"([{"name":"Eco","setpoint":18,"mode":"dry"}])",
                "Preset 1: mode must be one of keep/off/heat/cool/heat_cool"},
           Case{LIVING_ROOM, R"([{"name":"Eco","setpoint":18,"mode":"KEEP"}])",
                "Preset 1: mode must be one of keep/off/heat/cool/heat_cool"},
           Case{LIVING_ROOM, R"([{"name":"Eco","setpoint":18,"mode":1}])",
                "Preset 1: mode must be one of keep/off/heat/cool/heat_cool"},
           Case{LIVING_ROOM, R"([{"name":"Eco"}])", "Preset 1: setpoint must be a number"},
           Case{LIVING_ROOM, R"([{"name":"Eco","setpoint":"18"}])", "Preset 1: setpoint must be a number"},
           Case{LIVING_ROOM, R"([{"name":"Eco","setpoint":18,"mode":"cool"}])",
                "Preset 1: mode 'cool' needs cool.relay_id"},
           Case{LIVING_ROOM, R"([{"name":"Eco","setpoint":18,"mode":"heat_cool"}])",
                "Preset 1: mode 'heat_cool' needs both relays"},
           Case{cellar, R"([{"name":"Eco","setpoint":18,"mode":"heat"}])", "Preset 1: mode 'heat' needs heat.relay_id"},
           Case{LIVING_ROOM, R"([{"key":"Eco","name":"Eco","setpoint":18}])",
                "Preset 1: key must be a slug: lowercase letters, digits and single dashes"},
           Case{LIVING_ROOM, R"([{"key":"eco","name":"Eco","setpoint":18},{"key":"eco","name":"Away","setpoint":12}])",
                "Preset 2: key 'eco' is already used by Preset 1"},
           Case{LIVING_ROOM, R"([{"setpoint":18}])", "Preset 1: Name is required"},
           Case{LIVING_ROOM, R"([{"name":5,"setpoint":18}])", "Preset 1: Name is required"},
           Case{LIVING_ROOM, R"([{"name":"   ","setpoint":18}])", "Preset 1: Name is required"},
           Case{LIVING_ROOM, R"([{"name":")" + std::string(49, 'a') + R"(","setpoint":18}])",
                "Preset 1: Name is longer than 48 characters"},
           Case{LIVING_ROOM, R"([{"name":"Ночь","setpoint":18}])", "Preset 1: Use printable ASCII characters only"},
           Case{LIVING_ROOM, R"([{"name":"Up/down","setpoint":18}])", "Preset 1: Name cannot contain '/'"},
           Case{LIVING_ROOM, R"([{"name":"a\\b","setpoint":18}])", "Preset 1: Name cannot contain '\\'"},
           Case{LIVING_ROOM, R"([{"name":"Eco","setpoint":18},{"name":" NONE ","setpoint":5}])",
                "Preset 2: \"NONE\" is reserved"},
           Case{LIVING_ROOM, R"([{"name":"Eco","setpoint":18},{"name":"ECO","setpoint":5}])",
                "Preset 2: \"ECO\" is already used by Preset 1"},
           Case{LIVING_ROOM, R"([{"name":"Night Time","setpoint":18},{"name":"night   time","setpoint":5}])",
                "Preset 2: \"night   time\" is already used by Preset 1"},
       }) {
    Reply reply = this->post("save", with(c.body, R"("presets":)" + c.presets));
    EXPECT_EQ(reply.code, 400) << c.presets;
    EXPECT_EQ(reply.error(), c.error) << c.presets;
  }
  EXPECT_TRUE(this->files().empty());
}

// The thermostat's own rules first, then the list's structure, every preset's values, every
// preset's name, and the thermostat's name last.
TEST_F(Editor, APresetRefusalComesInTheDevicesOrder) {
  struct Case {
    std::string body;
    const char *error;
  };
  for (const Case &c : {
           Case{R"({"name":"A","heat":{"relay_id":"relay_1"},"presets":{}})", "sensor_id is required"},
           Case{R"({"name":"A","sensor_id":"room","heat":{"relay_id":"relay_1"},"mode":"cool","presets":{}})",
                "mode 'cool' needs cool.relay_id"},
           Case{with(LIVING_ROOM, R"("presets":[{"name":"Eco"},{"name":"Day","setpoint":20,"mode":"dry"}])"),
                "Preset 2: mode must be one of keep/off/heat/cool/heat_cool"},
           Case{with(LIVING_ROOM, R"("presets":[{"name":"Eco"},7])"), "Preset 2 must be an object"},
           Case{with(LIVING_ROOM, R"("presets":[{"name":"","setpoint":18},{"name":"Day"}])"),
                "Preset 2: setpoint must be a number"},
           Case{with(LIVING_ROOM, R"("presets":[{"name":"none","setpoint":18,"key":"X"}])"),
                "Preset 1: key must be a slug: lowercase letters, digits and single dashes"},
           Case{R"({"name":"Up/down","sensor_id":"room","heat":{"relay_id":"relay_1"},)"
                R"("presets":[{"name":"none","setpoint":5}]})",
                "Preset 1: \"none\" is reserved"},
           Case{R"({"name":"Up/down","sensor_id":"room","heat":{"relay_id":"relay_1"},)"
                R"("presets":[{"name":"Eco","setpoint":5}]})",
                "Name cannot contain '/'"},
       }) {
    Reply reply = this->post("save", c.body);
    EXPECT_EQ(reply.code, 400) << c.body;
    EXPECT_EQ(reply.error(), c.error) << c.body;
  }
}

// Picked from Home Assistant, the hub or by hand, and kept while the thermostat is stopped.
TEST_F(Editor, ListAndStatusCarryTheActivePreset) {
  ASSERT_EQ(this->create(STUDIO), "studio");
  ASSERT_EQ(this->create(with(LIVING_ROOM, R"("enabled":false)").c_str()), "living-room");
  auto active = [this](const char *route, size_t row) {
    Reply reply = this->get(route);
    JsonObject state = reply["controllers"][row];
    EXPECT_TRUE(state["active_preset"].is<const char *>()) << route << ": " << reply.body;
    EXPECT_TRUE(state["active_preset_name"].is<const char *>()) << route << ": " << reply.body;
    return state["active_preset"].as<std::string>() + "|" + state["active_preset_name"].as<std::string>();
  };
  // Sorted by id: living-room first.
  EXPECT_EQ(active("list", 1), "|");
  EXPECT_EQ(active("status", 1), "|");
  EXPECT_EQ(active("list", 0), "|") << "a thermostat with no presets carries the keys empty";

  ASSERT_TRUE(hub().apply_preset("studio", "night-time").ok);
  EXPECT_EQ(active("list", 1), "night-time|Night  Time");
  EXPECT_EQ(active("status", 1), "night-time|Night  Time");
  EXPECT_EQ(this->get("list")["controllers"][1]["mode"].as<std::string>(), "heat_cool");
  EXPECT_FLOAT_EQ(this->get("status?id=studio")["controllers"][0]["setpoint"].as<float>(), 19.5f);

  climate::Climate *entity = entity_named("Studio");
  ASSERT_NE(entity, nullptr);
  auto call = entity->make_call();
  call.set_preset("ECO");
  call.perform();
  EXPECT_EQ(active("status?id=studio", 0), "eco|Eco");
  EXPECT_FLOAT_EQ(this->get("status?id=studio")["controllers"][0]["setpoint"].as<float>(), 18.f);

  ASSERT_EQ(this->post("setpoint?id=studio&value=23").code, 200);
  EXPECT_EQ(active("status?id=studio", 0), "eco|Eco") << "a target set by hand keeps the label";

  ASSERT_EQ(this->post("enable?id=studio&value=false").code, 200);
  EXPECT_EQ(active("list", 1), "eco|Eco");
  EXPECT_EQ(active("status?id=studio", 0), "eco|Eco");
}

// The limits a form keeps to, and the words it may send: each one goes through a Save as it is.
TEST_F(Editor, SchemaDescribesThePresets) {
  Reply reply = this->get("schema");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(text_of(reply["presets"]), R"({"max_count":8,"modes":["keep","off","heat","cool","heat_cool"],)"
                                       R"("standard":["eco","away","boost","comfort","home","sleep","activity"]})");
  EXPECT_EQ(reply["presets"]["max_count"].as<size_t>(), climate_hub::PRESET_MAX_COUNT);
  size_t i = 0;
  for (const climate_hub::StandardPreset &standard : climate_hub::STANDARD_PRESETS)
    EXPECT_EQ(reply["presets"]["standard"][i++].as<std::string>(), standard.name);

  JsonDocument doc;
  deserializeJson(doc, STUDIO);
  JsonArray presets = doc["presets"].to<JsonArray>();
  for (JsonVariant name : reply["presets"]["standard"].as<JsonArray>()) {
    JsonObject preset = presets.add<JsonObject>();
    preset["name"] = name;
    preset["setpoint"] = 20;
    preset["mode"] = reply["presets"]["modes"][presets.size() % reply["presets"]["modes"].size()];
  }
  Reply saved = this->post("save", body_of(doc));
  ASSERT_EQ(saved.code, 200) << saved.body;
  Reply got = this->get("get?id=studio");
  ASSERT_EQ(got["presets"].size(), 7u);
  for (size_t n = 0; n < 7; n++) {
    EXPECT_EQ(got["presets"][n]["name"].as<std::string>(), doc["presets"][n]["name"].as<std::string>());
    EXPECT_EQ(got["presets"][n]["mode"].as<std::string>(), doc["presets"][n]["mode"].as<std::string>());
  }
}

// It runs and answers like any other; only a Save is refused, since it would drop what this
// firmware does not know. Changes stay in memory, and deleting it still removes the file.
TEST_F(Editor, AThermostatANewerFirmwareWroteRefusesASave) {
  const std::string text =
      R"({"version":3,"id":"boiler","name":"Boiler","kind":"bang_bang","sensor_id":"room",)"
      R"("heat":{"relay_id":"relay_1"},"mode":"heat","setpoint":21,"future":{"x":1},)"
      R"("presets":[{"key":"eco","name":"Eco","setpoint":18,"mode":"keep"}],"active_preset":"eco"})";
  mkdir(this->folder().c_str(), 0755);
  std::ofstream(this->folder() + "/boiler.json") << text;
  hub().reset();
  hub().setup();
  ASSERT_TRUE(hub().is_running("boiler"));

  Reply got = this->get("get?id=boiler");
  ASSERT_EQ(got.code, 200) << got.body;
  EXPECT_EQ(got["version"].as<int>(), 3);
  EXPECT_EQ(got["active_preset"].as<std::string>(), "eco");
  EXPECT_EQ(this->get("list")["controllers"][0]["active_preset_name"].as<std::string>(), "Eco");

  Reply refused = this->post("save", got.body);
  EXPECT_EQ(refused.code, 409);
  EXPECT_EQ(refused.error(), "A newer firmware wrote this thermostat; update the firmware to change it");
  // A body that breaks a rule is refused as such first, as for any thermostat.
  refused = this->post("save", R"({"id":"boiler","name":"Boiler"})");
  EXPECT_EQ(refused.code, 400);
  EXPECT_EQ(refused.error(), "sensor_id is required");

  ASSERT_EQ(this->post("setpoint?id=boiler&value=23").code, 200);
  EXPECT_FLOAT_EQ(this->get("get?id=boiler")["setpoint"].as<float>(), 23.f) << "in memory";
  Reply stopped = this->post("enable?id=boiler&value=false");
  ASSERT_EQ(stopped.code, 200) << stopped.body;
  EXPECT_EQ(stopped.body, R"({"success":true,"message":"Thermostat disabled","persisted":false})");
  Reply started = this->post("enable?id=boiler&value=true");
  ASSERT_EQ(started.code, 200) << started.body;
  EXPECT_EQ(started.body, R"({"success":true,"message":"Thermostat enabled","persisted":false})");
  hub().ms += 3000;
  hub().loop();
  EXPECT_EQ(this->file("boiler.json"), text);

  Reply removed = this->post("delete?id=boiler");
  ASSERT_EQ(removed.code, 200) << removed.body;
  EXPECT_EQ(removed.body, R"({"success":true,"message":"Thermostat deleted","persisted":true})");
  EXPECT_TRUE(this->files().empty());
}

}  // namespace esphome::web_climate_editor::testing
