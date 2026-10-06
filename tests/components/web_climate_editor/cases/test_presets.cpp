#include "common.h"

// The presets over HTTP where only the device can show them: a pick reaching the climate entity and
// the file, one from Home Assistant, the body as get writes it, the schema against the hub's tables,
// and a file a newer firmware wrote. The requests the client mock answers too are in
// ../contract.json.
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

std::string body_of(const JsonDocument &doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

// What the entity says is active: the built-in preset's name, the custom one's, or "".
std::string label(climate::Climate *entity) {
  if (entity->has_custom_preset())
    return entity->get_custom_preset().c_str();
  if (entity->preset.has_value())
    return LOG_STR_ARG(climate::climate_preset_to_string(*entity->preset));
  return "";
}

// Past the hub's delay, so what waited to be written is in the file.
void flush() {
  hub().ms += 3000;
  hub().loop();
}

// The entity a running thermostat drives, nullptr when it does not run.
climate::Climate *entity_of(const char *id) {
  const climate_hub::ControllerRuntime *runtime = hub().runtime(id);
  return runtime != nullptr ? runtime->entity() : nullptr;
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

// A pick over HTTP takes the path one from Home Assistant takes: the entity shows it at once, and
// the file follows with its target and its mode.
TEST_F(Editor, APresetPickedOverHttpReachesTheEntityAndTheFile) {
  ASSERT_EQ(this->create(STUDIO), "studio");
  climate::Climate *entity = entity_of("studio");
  ASSERT_NE(entity, nullptr);
  Reply picked = this->post("preset?id=studio&key=night-time");
  ASSERT_EQ(picked.code, 200) << picked.body;
  EXPECT_EQ(picked.body, R"({"success":true,"message":"Preset applied","persisted":true})");
  EXPECT_EQ(label(entity), "Night  Time");
  EXPECT_FLOAT_EQ(entity->target_temperature, 19.5f);
  EXPECT_EQ(entity->mode, climate::CLIMATE_MODE_HEAT_COOL);
  flush();
  const std::string file = this->file("studio.json");
  EXPECT_NE(file.find(R"("mode":"heat_cool")"), std::string::npos) << file;
  EXPECT_NE(file.find(R"("setpoint":19.5,"presets")"), std::string::npos) << file;
  EXPECT_NE(file.find(R"("active_preset":"night-time"})"), std::string::npos) << file;

  ASSERT_EQ(this->post("preset?id=studio&key=eco").code, 200);
  EXPECT_EQ(label(entity), "ECO") << "a built-in one is Home Assistant's own";
}

// Stopped, it has no entity to show the pick: the document keeps it, and the entity it starts in
// shows it.
TEST_F(Editor, AThermostatStartsInThePresetPickedWhileItWasStopped) {
  ASSERT_EQ(this->create(STUDIO), "studio");
  ASSERT_EQ(this->post("enable?id=studio&value=false").code, 200);
  Reply picked = this->post("preset?id=studio&key=away");
  ASSERT_EQ(picked.code, 200) << picked.body;
  EXPECT_EQ(picked.body, R"({"success":true,"message":"Preset applied","persisted":true})");
  flush();
  const std::string file = this->file("studio.json");
  EXPECT_NE(file.find(R"("mode":"off","last_on_mode":"heat","setpoint":12,)"), std::string::npos) << file;
  EXPECT_NE(file.find(R"("active_preset":"away"})"), std::string::npos) << file;

  ASSERT_EQ(this->post("enable?id=studio&value=true").code, 200);
  climate::Climate *entity = entity_of("studio");
  ASSERT_NE(entity, nullptr);
  EXPECT_EQ(label(entity), "AWAY");
  EXPECT_FLOAT_EQ(entity->target_temperature, 12.f);
  EXPECT_EQ(entity->mode, climate::CLIMATE_MODE_OFF);
}

// Picked through the entity, as Home Assistant and web_server pick one, and read back over HTTP.
TEST_F(Editor, APresetPickedFromHomeAssistantShowsInListAndStatus) {
  ASSERT_EQ(this->create(STUDIO), "studio");
  climate::Climate *entity = entity_of("studio");
  ASSERT_NE(entity, nullptr);
  auto eco = entity->make_call();
  eco.set_preset("ECO");
  eco.perform();
  Reply status = this->get("status?id=studio");
  EXPECT_EQ(status["controllers"][0]["active_preset"].as<std::string>(), "eco");
  EXPECT_EQ(status["controllers"][0]["active_preset_name"].as<std::string>(), "Eco");
  EXPECT_FLOAT_EQ(status["controllers"][0]["setpoint"].as<float>(), 18.f);

  auto night = entity->make_call();
  night.set_preset("Night  Time");
  night.perform();
  Reply list = this->get("list");
  EXPECT_EQ(list["controllers"][0]["active_preset"].as<std::string>(), "night-time");
  EXPECT_EQ(list["controllers"][0]["active_preset_name"].as<std::string>(), "Night  Time");
  EXPECT_EQ(list["controllers"][0]["mode"].as<std::string>(), "heat_cool");
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
  const unsigned newer = climate_hub::CONFIG_VERSION + 1;
  const std::string text =
      R"({"version":)" + std::to_string(newer) +
      R"(,"id":"boiler","name":"Boiler","kind":"bang_bang","sensor_id":"room",)"
      R"("heat":{"relay_id":"relay_1"},"mode":"heat","setpoint":21,"future":{"x":1},)"
      R"("presets":[{"key":"eco","name":"Eco","setpoint":18,"mode":"keep"}],"active_preset":"eco"})";
  mkdir(this->folder().c_str(), 0755);
  std::ofstream(this->folder() + "/boiler.json") << text;
  hub().reset();
  hub().setup();
  ASSERT_TRUE(hub().is_running("boiler"));

  Reply got = this->get("get?id=boiler");
  ASSERT_EQ(got.code, 200) << got.body;
  EXPECT_EQ(got["version"].as<unsigned>(), newer);
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
  Reply picked = this->post("preset?id=boiler&key=eco");
  ASSERT_EQ(picked.code, 200) << picked.body;
  EXPECT_EQ(picked.body, R"({"success":true,"message":"Preset applied","persisted":false})");
  EXPECT_FLOAT_EQ(this->get("get?id=boiler")["setpoint"].as<float>(), 18.f) << "in memory";
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
