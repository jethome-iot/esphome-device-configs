#include "common.h"
#include "esphome/components/climate_hub/param_table.h"

// Every route's answer, and its refusals in the order client/mock/climateMock.ts gives them:
// the dashboard is built against that mock, so a refusal that comes out differently here is a
// different message on the device than in development.
namespace esphome::web_climate_editor::testing {

// --- list, get, save ---

TEST_F(Editor, AnEmptyHubListsNothing) {
  Reply reply = this->get("list");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, R"({"success":true,"count":0,"max_controllers":3,"controllers":[]})");
}

TEST_F(Editor, SaveCreatesAThermostatTheOtherRoutesThenSee) {
  Reply saved = this->post("save", LIVING_ROOM);
  ASSERT_EQ(saved.code, 200) << saved.body;
  EXPECT_EQ(saved.body, R"({"success":true,"message":"Thermostat created","id":"living-room"})");
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
  EXPECT_TRUE(hub().is_running("living-room"));

  Reply list = this->get("list");
  EXPECT_EQ(list.body, R"({"success":true,"count":1,"max_controllers":3,"controllers":[)"
                       R"({"id":"living-room","name":"Living Room","enabled":true,"kind":"pid","mode":"heat",)"
                       R"("sensor_id":"room","heat_relay_id":"relay_1","cool_relay_id":"","running":true,)"
                       R"("waiting":""}]})");

  // The bare document, every key there, the same shape save takes back.
  Reply got = this->get("get?id=living-room");
  ASSERT_EQ(got.code, 200) << got.body;
  EXPECT_TRUE(got["success"].isUnbound());
  EXPECT_EQ(got["version"].as<int>(), 1);
  EXPECT_EQ(got["id"].as<std::string>(), "living-room");
  EXPECT_EQ(got["name"].as<std::string>(), "Living Room");
  EXPECT_FLOAT_EQ(got["setpoint"].as<float>(), 22.f);
  EXPECT_EQ(got["heat"]["min_on_s"].as<int>(), 10);
  EXPECT_EQ(got["cool"]["relay_id"].as<std::string>(), "");
  for (const char *key :
       {"enabled", "kind", "sensor_id", "update_interval_s", "visual", "safety", "pid", "bang_bang", "mode"})
    EXPECT_FALSE(got[key].isUnbound()) << key;
}

// A rename keeps the id, and with it the file: only the entity's name moves.
TEST_F(Editor, SaveWithAnIdReplacesThatThermostat) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  Reply got = this->get("get?id=living-room");
  got.json["name"] = "Lounge";
  got.json["setpoint"] = 19.5;
  std::string body;
  serializeJson(got.json, body);

  Reply saved = this->post("save", body);
  ASSERT_EQ(saved.code, 200) << saved.body;
  EXPECT_EQ(saved.body, R"({"success":true,"message":"Thermostat updated","id":"living-room"})");
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
  EXPECT_EQ(hub().store().get("living-room")->name, "Lounge");
  EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, 19.5f);
  EXPECT_EQ(this->get("list")["controllers"].size(), 1u);
}

// "" is what the client sends for a create, so a copied document cannot turn into an update.
TEST_F(Editor, AnEmptyIdCreates) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  Reply saved = this->post("save", with(FLOOR, R"("id":"")"));
  ASSERT_EQ(saved.code, 200) << saved.body;
  EXPECT_EQ(saved.message(), "Thermostat created");
  EXPECT_EQ(saved["id"].as<std::string>(), "floor");
}

// Only a string picks a thermostat to replace: a number is no id, so the save creates.
TEST_F(Editor, ANumericIdCreates) {
  Reply saved = this->post("save", with(LIVING_ROOM, R"("id":5)"));
  ASSERT_EQ(saved.code, 200) << saved.body;
  EXPECT_EQ(saved.message(), "Thermostat created");
  EXPECT_EQ(saved["id"].as<std::string>(), "living-room");
}

TEST_F(Editor, APartialDocumentTakesTheDefaults) {
  Reply saved = this->post("save", R"({"name":"Porch","sensor_id":"room","heat":{"relay_id":"relay_1"}})");
  ASSERT_EQ(saved.code, 200) << saved.body;
  Reply got = this->get("get?id=porch");
  EXPECT_EQ(got["kind"].as<std::string>(), "bang_bang");
  EXPECT_EQ(got["mode"].as<std::string>(), "heat");
  EXPECT_FLOAT_EQ(got["setpoint"].as<float>(), 21.f);
  EXPECT_EQ(got["heat"]["min_off_s"].as<int>(), 10);
}

TEST_F(Editor, SaveRefusesWhatItCannotRead) {
  struct Case {
    std::string body;
    const char *content_type;
    const char *error;
  };
  for (const Case &c : {
           Case{"", "application/json", "Empty request body"},
           // A form is parsed into fields by the server and never reaches the body.
           Case{LIVING_ROOM, "application/x-www-form-urlencoded", "Empty request body"},
           Case{R"({"name":"Broken",)", "application/json", "JSON parse error: IncompleteInput"},
           Case{"garbage", "application/json", "JSON parse error: InvalidInput"},
           Case{"[]", "application/json", "document is not an object"},
           Case{R"({"sensor_id":"room","heat":{"relay_id":"relay_1"}})", "application/json", "name is required"},
           Case{R"({"name":"   ","sensor_id":"room","heat":{"relay_id":"relay_1"}})", "application/json",
                "Name is required"},
           Case{R"({"name":"Up/down","sensor_id":"room","heat":{"relay_id":"relay_1"}})", "application/json",
                "Name cannot contain '/'"},
           Case{R"({"name":"Кухня","sensor_id":"room","heat":{"relay_id":"relay_1"}})", "application/json",
                "Use printable ASCII characters only"},
           Case{R"({"name":")" + std::string(49, 'a') + R"(","sensor_id":"room","heat":{"relay_id":"relay_1"}})",
                "application/json", "Name is longer than 48 characters"},
           // The codec's rules come first, the name's last.
           Case{R"({"name":"Up/down","heat":{"relay_id":"relay_1"}})", "application/json", "sensor_id is required"},
           Case{R"({"name":"A","sensor_id":"room","heat":{"relay_id":"relay_1"},"mode":"dry"})", "application/json",
                "mode must be one of off/heat/cool/heat_cool"},
           // An entity named by a number is no entity: ArduinoJson would hand over its JSON text.
           Case{R"({"name":"A","sensor_id":5,"heat":{"relay_id":"relay_1"}})", "application/json",
                "sensor_id is required"},
           Case{R"({"name":"A","sensor_id":"room","heat":{"relay_id":7}})", "application/json",
                "at least one of heat.relay_id / cool.relay_id is required"},
           // No object id is longer; an uncapped one could grow the file past what a boot loads.
           Case{R"({"name":"A","sensor_id":")" + std::string(121, 'a') + R"(","heat":{"relay_id":"relay_1"}})",
                "application/json", "sensor_id is longer than 120 characters"},
       }) {
    Reply reply = this->call(HTTP_POST, "save", c.body, c.content_type);
    EXPECT_EQ(reply.code, 400) << c.body;
    EXPECT_EQ(reply.error(), c.error) << c.body;
  }
  EXPECT_TRUE(this->files().empty());
  EXPECT_EQ(hub().store().size(), 0u);
}

TEST_F(Editor, SaveRefusesABodyOverEightKiB) {
  const std::string padded = with(LIVING_ROOM, R"("note":")" + std::string(8192, 'x') + "\"");
  Reply reply = this->post("save", padded);
  EXPECT_EQ(reply.code, 413);
  EXPECT_EQ(reply.error(), "Request body over 8 KiB");
  EXPECT_TRUE(this->files().empty());
  // The refusal leaves nothing behind for the next request.
  EXPECT_EQ(this->create(LIVING_ROOM), "living-room");
}

// The cap is the document's size, counted over every chunk the server hands over: 8 KiB is
// read, a byte more is not.
TEST_F(Editor, EightKiBIsTheLargestBodySaveReads) {
  const std::string living_room = LIVING_ROOM;
  Reply fits = this->post("save", living_room + std::string(8192 - living_room.size(), ' '));
  EXPECT_EQ(fits.code, 200) << fits.body;
  EXPECT_EQ(fits["id"].as<std::string>(), "living-room");

  const std::string floor = FLOOR;
  Reply over = this->post("save", floor + std::string(8193 - floor.size(), ' '));
  EXPECT_EQ(over.code, 413);
  EXPECT_EQ(over.type, "application/json");
  EXPECT_EQ(over.error(), "Request body over 8 KiB");
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
}

// Only save reads a body, so only save can be too large for one.
TEST_F(Editor, OnlySaveMindsTheBodySize) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  Reply reply = this->post("setpoint?id=living-room&value=23", std::string(9000, 'x'));
  EXPECT_EQ(reply.code, 200) << reply.body;
}

// web_server_idf never reaches handleRequest when the receive fails midway, so the buffer
// outlives that request; the next one must not read it as its own.
TEST_F(Editor, ABodyLeftByAFailedReceiveIsNotTheNextRequests) {
  std::string partial = LIVING_ROOM;
  AsyncWebServerRequest aborted(HTTP_POST, "/climate-editor/api/save", partial);
  this->editor->handleBody(&aborted, reinterpret_cast<uint8_t *>(&partial[0]), 20, 0, partial.size());

  Reply reply = this->post("save");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Empty request body");

  // A form of the same length never reaches handleBody; the leftover must not pass for it.
  this->editor->handleBody(&aborted, reinterpret_cast<uint8_t *>(&partial[0]), 20, 0, partial.size());
  reply = this->call(HTTP_POST, "save", std::string(partial.size(), 'x'), "application/x-www-form-urlencoded");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Empty request body");
  EXPECT_TRUE(this->files().empty());
}

// What a failed receive left is dropped as soon as the next body starts.
TEST_F(Editor, ANewBodyReplacesWhatAFailedReceiveLeft) {
  std::string partial = FLOOR;
  AsyncWebServerRequest aborted(HTTP_POST, "/climate-editor/api/save", partial);
  this->editor->handleBody(&aborted, reinterpret_cast<uint8_t *>(&partial[0]), 20, 0, partial.size());

  Reply reply = this->post("save", LIVING_ROOM);
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply["id"].as<std::string>(), "living-room");
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
}

TEST_F(Editor, SaveWithAnUnknownIdIsNotFound) {
  Reply reply = this->post("save", with(LIVING_ROOM, R"("id":"ghost")"));
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Thermostat not found");
  // A broken document is refused as such first.
  reply = this->post("save", R"({"id":"ghost","name":"Ghost"})");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "sensor_id is required");
  EXPECT_TRUE(this->files().empty());
}

TEST_F(Editor, SaveRefusesANameAnotherClimateAnswersTo) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  struct Case {
    const char *name;
    const char *error;
  };
  for (const Case &c : {
           Case{"living  ROOM", "\"living  ROOM\" is already used by another thermostat"},
           Case{"Living_Room",
                "\"Living_Room\" is too close to \"Living Room\": both are living_room to Home Assistant"},
           // The web server finds a climate by name, the YAML's first.
           Case{"Hall", "\"Hall\" is already used by another thermostat"},
       }) {
    std::string body = FLOOR;
    body.replace(body.find("Floor"), 5, c.name);
    Reply reply = this->post("save", body);
    EXPECT_EQ(reply.code, 409) << c.name;
    EXPECT_EQ(reply.error(), c.error) << c.name;
  }
  // Its own name never blocks a thermostat.
  Reply same = this->post("save", with(LIVING_ROOM, R"("id":"living-room")"));
  EXPECT_EQ(same.code, 200) << same.body;
}

TEST_F(Editor, SaveStopsAtTheLimitBeforeItLooksAtTheName) {
  auto named = [](const char *name) {
    std::string body = with(LIVING_ROOM, R"("enabled":false)");
    return body.replace(body.find("Living Room"), 11, name);
  };
  for (const char *name : {"One", "Two", "Three"})
    ASSERT_EQ(this->post("save", named(name)).code, 200) << name;
  Reply reply = this->post("save", named("One"));
  EXPECT_EQ(reply.code, 507);
  EXPECT_EQ(reply.error(), "This device allows 3 thermostats; delete one to add another");
  EXPECT_EQ(this->files().size(), 3u);
}

// Files the hub left alone keep their ids; when they hold every id a name gives, the create is
// a 409 that says so, and none of them is written over.
TEST_F(Editor, SaveRefusesANameWhoseIdsAreAllTakenByFiles) {
  // Every id a create tries: climate_hub.cpp's MAX_ID_SUFFIX.
  for (unsigned n = 1; n <= 999; n++) {
    std::ofstream(this->folder() + "/" + climate_hub::id_with_suffix("living-room", n) + ".json") << "left alone";
  }
  Reply reply = this->post("save", LIVING_ROOM);
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(),
            "Every id made from \"Living Room\" is taken by a file in the thermostat folder; choose another name");
  EXPECT_EQ(hub().store().size(), 0u);
  EXPECT_EQ(this->file("living-room.json"), "left alone");
  EXPECT_EQ(this->file("living-room-999.json"), "left alone");
}

// A sensor or relay the device does not have is no refusal: the thermostat is stored enabled
// and waits for it, and the answer says so in a key of its own as well as in the message.
TEST_F(Editor, AnEnabledThermostatWhoseSensorOrRelayIsMissingIsSavedAndWaits) {
  std::string attic = LIVING_ROOM;
  attic.replace(attic.find("\"room\""), 6, "\"attic\"");
  Reply created = this->post("save", attic);
  ASSERT_EQ(created.code, 200) << created.body;
  EXPECT_EQ(created.body, R"({"success":true,"message":"Thermostat created; not started: sensor 'attic' not found",)"
                          R"("id":"living-room","warning":"not started: sensor 'attic' not found"})");
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
  EXPECT_TRUE(hub().store().get("living-room")->enabled);
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_EQ(hub().claimed_by("relay_1"), "");

  // Waiting shows as enabled and not running, with no reading, nothing driven, no fault, and
  // the warning the Save gave.
  Reply list = this->get("list");
  JsonObject row = list["controllers"][0];
  EXPECT_TRUE(row["enabled"].as<bool>());
  EXPECT_FALSE(row["running"].as<bool>());
  EXPECT_EQ(row["waiting"].as<std::string>(), "not started: sensor 'attic' not found");
  Reply status = this->get("status?id=living-room");
  JsonObject state = status["controllers"][0];
  EXPECT_FALSE(state["running"].as<bool>());
  EXPECT_EQ(state["waiting"].as<std::string>(), "not started: sensor 'attic' not found");
  EXPECT_EQ(state["action"].as<std::string>(), "off");
  EXPECT_EQ(state["fault"].as<std::string>(), "none");
  EXPECT_TRUE(state["current_temperature"].isNull());
  EXPECT_FALSE(state["heat_relay_on"].as<bool>());

  std::string no_relay = with(LIVING_ROOM, R"("id":"living-room")");
  no_relay.replace(no_relay.find("relay_1"), 7, "relay_9");
  Reply updated = this->post("save", no_relay);
  ASSERT_EQ(updated.code, 200) << updated.body;
  EXPECT_EQ(updated.message(), "Thermostat updated; not started: relay 'relay_9' not found");
  EXPECT_EQ(updated["warning"].as<std::string>(), "not started: relay 'relay_9' not found");
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_EQ(this->get("list")["controllers"][0]["waiting"].as<std::string>(), "not started: relay 'relay_9' not found");

  // An internal sensor is no input, so it is as missing as one the device never had.
  std::string hidden = with(LIVING_ROOM, R"("id":"living-room")");
  hidden.replace(hidden.find("\"room\""), 6, "\"probe\"");
  EXPECT_EQ(this->post("save", hidden)["warning"].as<std::string>(), "not started: sensor 'probe' not found");

  // Named what is there, the next Save starts it, and the answer carries no warning.
  Reply fixed = this->post("save", with(LIVING_ROOM, R"("id":"living-room")"));
  ASSERT_EQ(fixed.code, 200) << fixed.body;
  EXPECT_EQ(fixed.body, R"({"success":true,"message":"Thermostat updated","id":"living-room"})");
  EXPECT_TRUE(hub().is_running("living-room"));
  EXPECT_EQ(this->get("list")["controllers"][0]["waiting"].as<std::string>(), "");
  EXPECT_EQ(this->get("status")["controllers"][0]["waiting"].as<std::string>(), "");
}

// The hub stops a running thermostat a Save moves onto what is not there, and lets its relay go.
TEST_F(Editor, ASaveOntoAMissingSensorStopsTheThermostat) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  hub().loop();
  entities().room.publish_state(15.f);
  hub().loop();
  ASSERT_TRUE(entities().relay1.state);

  std::string attic = with(LIVING_ROOM, R"("id":"living-room")");
  attic.replace(attic.find("\"room\""), 6, "\"attic\"");
  Reply reply = this->post("save", attic);
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply["warning"].as<std::string>(), "not started: sensor 'attic' not found");
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_TRUE(hub().store().get("living-room")->enabled);
  EXPECT_EQ(hub().claimed_by("relay_1"), "");
  EXPECT_FALSE(entities().relay1.state);
}

// What a Save of an enabled thermostat still refuses: a sensor that is there but not in °C, and
// a relay a running thermostat holds, even when the sensor is missing as well.
TEST_F(Editor, AnEnabledSaveRefusesASensorNotInCelsiusAndAHeldRelay) {
  std::string not_celsius = LIVING_ROOM;
  not_celsius.replace(not_celsius.find("\"room\""), 6, "\"uptime\"");
  Reply reply = this->post("save", not_celsius);
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "\"Uptime\" reports s, not °C");
  EXPECT_TRUE(this->files().empty());

  // A disabled one may name it: it is not about to run.
  EXPECT_EQ(this->post("save", with(not_celsius.c_str(), R"("enabled":false)")).code, 200);

  ASSERT_EQ(this->create(FLOOR), "floor");
  std::string shares = LIVING_ROOM;
  shares.replace(shares.find("Living Room"), 11, "Kitchen");
  shares.replace(shares.find("relay_1"), 7, "relay_2");
  reply = this->post("save", shares);
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "\"Relay 2\" is already driven by \"Floor\"");

  shares.replace(shares.find("\"room\""), 6, "\"attic\"");
  reply = this->post("save", shares);
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "\"Relay 2\" is already driven by \"Floor\"");
  EXPECT_EQ(this->files().size(), 2u);
  EXPECT_EQ(hub().store().get("kitchen"), nullptr);
}

// A document that breaks three rules gets the first in the README's order: the name, then the
// sensor's unit, then the held relay. Each fix brings the next one to light.
TEST_F(Editor, TheRefusalsComeInTheDocumentedOrder) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  std::string doc = LIVING_ROOM;
  doc.replace(doc.find("Living Room"), 11, "living room");
  doc.replace(doc.find("\"room\""), 6, "\"uptime\"");

  Reply reply = this->post("save", doc);
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "\"living room\" is already used by another thermostat");

  doc.replace(doc.find("living room"), 11, "Kitchen");
  reply = this->post("save", doc);
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "\"Uptime\" reports s, not °C");

  doc.replace(doc.find("\"uptime\""), 8, "\"floor\"");
  reply = this->post("save", doc);
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "\"Relay 1\" is already driven by \"Living Room\"");
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
}

TEST_F(Editor, AFileThatCannotBeWrittenIsAServerError) {
  storage().set_base_path("/proc/definitely-not-writable");
  Reply reply = this->post("save", LIVING_ROOM);
  storage().set_base_path(this->base_path);
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "The thermostat's file could not be written");
  EXPECT_EQ(hub().store().size(), 0u);
}

// The hub never writes a file the next boot would refuse, and the editor says so with a 413.
TEST_F(Editor, AFileOverTheCapIsNotWritten) {
  hub().max_file_bytes = 100;
  Reply reply = this->post("save", LIVING_ROOM);
  EXPECT_EQ(reply.code, 413);
  EXPECT_EQ(reply.error(), "The thermostat's file would be over 8 KiB");
  EXPECT_TRUE(this->files().empty());
  EXPECT_EQ(hub().store().size(), 0u);
}

TEST_F(Editor, AnUpdateThatCannotBeWrittenChangesNothing) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  std::string lounge = with(LIVING_ROOM, R"("id":"living-room")");
  lounge.replace(lounge.find("Living Room"), 11, "Lounge");
  storage().set_base_path("/proc/definitely-not-writable");
  Reply reply = this->post("save", lounge);
  storage().set_base_path(this->base_path);
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "The thermostat's file could not be written");
  EXPECT_EQ(hub().store().get("living-room")->name, "Living Room");
  EXPECT_TRUE(hub().is_running("living-room"));
}

// Stored even when it cannot run, and the answer says why. The pool has an entity for every
// thermostat the device allows, so this is App having had no room for the whole pool.
TEST_F(Editor, AThermostatWithNoFreeEntityIsStoredAndTheAnswerSaysSo) {
  hub().take_every_slot();
  Reply created = this->post("save", LIVING_ROOM);
  ASSERT_EQ(created.code, 200) << created.body;
  EXPECT_EQ(created.message(), "Thermostat created; not started: no free climate entity");
  EXPECT_EQ(created["warning"].as<std::string>(), "not started: no free climate entity");
  EXPECT_EQ(created["id"].as<std::string>(), "living-room");
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_EQ(hub().claimed_by("relay_1"), "");

  Reply updated = this->post("save", with(LIVING_ROOM, R"("id":"living-room")"));
  ASSERT_EQ(updated.code, 200) << updated.body;
  EXPECT_EQ(updated.message(), "Thermostat updated; not started: no free climate entity");
  EXPECT_EQ(updated["warning"].as<std::string>(), "not started: no free climate entity");
  EXPECT_EQ(this->get("list")["controllers"][0]["waiting"].as<std::string>(), "not started: no free climate entity");

  ASSERT_EQ(this->post("enable?id=living-room&value=false").code, 200);
  EXPECT_EQ(this->get("list")["controllers"][0]["waiting"].as<std::string>(), "");
  Reply enabled = this->post("enable?id=living-room&value=true");
  ASSERT_EQ(enabled.code, 200) << enabled.body;
  EXPECT_EQ(enabled.message(), "Thermostat enabled; not started: no free climate entity");
  EXPECT_EQ(enabled["warning"].as<std::string>(), "not started: no free climate entity");
  EXPECT_TRUE(enabled["persisted"].as<bool>());
  EXPECT_TRUE(hub().store().get("living-room")->enabled);
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_EQ(this->get("status")["controllers"][0]["waiting"].as<std::string>(), "not started: no free climate entity");
}

// --- delete ---

TEST_F(Editor, DeleteRemovesTheThermostatAndItsFile) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  Reply reply = this->post("delete?id=living-room");
  EXPECT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat deleted","persisted":true})");
  EXPECT_TRUE(this->files().empty());
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_EQ(hub().claimed_by("relay_1"), "");

  reply = this->post("delete?id=living-room");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Thermostat not found");
}

// The loader refuses an empty file, so a file the partition will not unlink is emptied and the
// thermostat stays gone.
TEST_F(Editor, DeleteEmptiesAFileItCannotRemove) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  hub().refuse_remove = true;
  Reply reply = this->post("delete?id=living-room");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat deleted","persisted":true})");
  struct stat info {};
  ASSERT_EQ(stat((this->folder() + "/living-room.json").c_str(), &info), 0);
  EXPECT_EQ(info.st_size, 0);
  EXPECT_EQ(hub().store().get("living-room"), nullptr);
  EXPECT_EQ(hub().claimed_by("relay_1"), "");
}

// Gone until the next boot, and the answer says so rather than claim more than happened.
TEST_F(Editor, DeleteSaysWhenTheThermostatComesBackAtTheNextBoot) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes a read-only file";
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  ASSERT_EQ(chmod((this->folder() + "/living-room.json").c_str(), 0444), 0);
  hub().refuse_remove = true;
  Reply reply = this->post("delete?id=living-room");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.message(), "Thermostat deleted; its file could not be removed, so it comes back at the next boot");
  ASSERT_TRUE(reply["persisted"].is<bool>()) << reply.body;
  EXPECT_FALSE(reply["persisted"].as<bool>());
  EXPECT_EQ(hub().store().get("living-room"), nullptr);
  EXPECT_FALSE(hub().is_running("living-room"));
}

// An id is a file name: present, and a slug, before anything is looked up.
TEST_F(Editor, AnIdMustBeASlug) {
  for (const char *route : {"get", "delete"}) {
    Reply reply = this->call(std::string(route) == "get" ? HTTP_GET : HTTP_POST, route);
    EXPECT_EQ(reply.code, 400) << route;
    EXPECT_EQ(reply.error(), "Missing id parameter") << route;
  }
  const std::string long_id = std::string(49, 'a');
  for (const std::string &bad :
       {std::string(""), std::string("Living"), std::string("living room"), std::string("a--b"), std::string("-a"),
        std::string("a-"), std::string("../x"), long_id}) {
    Reply reply = this->get("get?id=" + bad);
    EXPECT_EQ(reply.code, 400) << bad;
    EXPECT_EQ(reply.error(), "Invalid id parameter") << bad;
  }
  Reply reply = this->get("get?id=" + std::string(48, 'a'));
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Thermostat not found");
}

// The routes that change something check it as strictly, before the hub is asked: unchecked, an
// id that is no slug would come back as an unknown thermostat's 404.
TEST_F(Editor, AWriteRouteRefusesAnIdThatIsNoSlug) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  hub().jobs = 0;
  for (const char *id : {"Living-Room", "living_room", "../living-room", ""}) {
    for (const std::string &route : {"delete?id=" + std::string(id), "enable?id=" + std::string(id) + "&value=false"}) {
      Reply reply = this->post(route);
      EXPECT_EQ(reply.code, 400) << route;
      EXPECT_EQ(reply.error(), "Invalid id parameter") << route;
    }
  }
  EXPECT_EQ(hub().jobs, 0);
  EXPECT_TRUE(hub().is_running("living-room"));
  EXPECT_EQ(this->files(), std::vector<std::string>{"living-room.json"});
}

// --- enable ---

TEST_F(Editor, EnableStopsAndStartsAThermostat) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  Reply reply = this->post("enable?id=living-room&value=false");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat disabled","persisted":true})");
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_FALSE(hub().store().get("living-room")->enabled);

  reply = this->post("enable?id=living-room&value=true");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat enabled","persisted":true})");
  EXPECT_TRUE(hub().is_running("living-room"));
}

TEST_F(Editor, EnableReadsItsParametersStrictly) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  struct Case {
    const char *route;
    int code;
    const char *error;
  };
  for (const Case &c : {
           Case{"enable?value=true", 400, "Missing id parameter"},
           Case{"enable?id=living-room", 400, "Missing value parameter"},
           Case{"enable?id=living-room&value=1", 400, "Invalid value parameter"},
           Case{"enable?id=living-room&value=TRUE", 400, "Invalid value parameter"},
           Case{"enable?id=living-room&value=true&take_over=yes", 400, "Invalid take_over parameter"},
           // The parameters are read before the id is looked up.
           Case{"enable?id=ghost&value=maybe", 400, "Invalid value parameter"},
           Case{"enable?id=ghost&value=true", 404, "Thermostat not found"},
       }) {
    Reply reply = this->post(c.route);
    EXPECT_EQ(reply.code, c.code) << c.route;
    EXPECT_EQ(reply.error(), c.error) << c.route;
  }
  EXPECT_EQ(this->post("enable?id=living-room&value=true&take_over=false").code, 200);
}

// Two thermostats on one relay take turns: the second starts only by taking the relay over,
// which stops the first and stores it as disabled.
TEST_F(Editor, EnableTakesARelayOverOnlyWhenAsked) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  std::string guest = with(LIVING_ROOM, R"("enabled":false)");
  guest.replace(guest.find("Living Room"), 11, "Guest Room");
  ASSERT_EQ(this->post("save", guest).code, 200);

  Reply refused = this->post("enable?id=guest-room&value=true");
  EXPECT_EQ(refused.code, 409);
  EXPECT_EQ(refused.error(), "\"Relay 1\" is already driven by \"Living Room\"");
  EXPECT_FALSE(hub().is_running("guest-room"));

  Reply taken = this->post("enable?id=guest-room&value=true&take_over=true");
  ASSERT_EQ(taken.code, 200) << taken.body;
  EXPECT_EQ(taken.message(), "Thermostat enabled; \"Living Room\" stopped");
  EXPECT_TRUE(taken["persisted"].as<bool>());
  EXPECT_TRUE(hub().is_running("guest-room"));
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_FALSE(hub().store().get("living-room")->enabled);
  EXPECT_EQ(hub().claimed_by("relay_1"), "guest-room");

  // Nothing to take over: the plain answer.
  Reply again = this->post("enable?id=guest-room&value=true&take_over=true");
  EXPECT_EQ(again.message(), "Thermostat enabled");
}

TEST_F(Editor, TakeOverIsReadStrictlyAndActsOnlyOnAStart) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  std::string guest = with(LIVING_ROOM, R"("enabled":false)");
  guest.replace(guest.find("Living Room"), 11, "Guest Room");
  ASSERT_EQ(this->post("save", guest).code, 200);

  for (const char *bad : {"", "TRUE", "True", "1", "yes", "true "}) {
    Reply reply = this->post(std::string("enable?id=guest-room&value=true&take_over=") + bad);
    EXPECT_EQ(reply.code, 400) << "'" << bad << "'";
    EXPECT_EQ(reply.error(), "Invalid take_over parameter") << "'" << bad << "'";
  }
  // Read on a stop too, though a stop takes nothing over.
  EXPECT_EQ(this->post("enable?id=living-room&value=false&take_over=maybe").error(), "Invalid take_over parameter");
  EXPECT_TRUE(hub().is_running("living-room"));

  Reply kept = this->post("enable?id=guest-room&value=true&take_over=false");
  EXPECT_EQ(kept.code, 409);
  EXPECT_EQ(kept.error(), "\"Relay 1\" is already driven by \"Living Room\"");
  Reply stopped = this->post("enable?id=guest-room&value=false&take_over=true");
  EXPECT_EQ(stopped.message(), "Thermostat disabled");
  EXPECT_TRUE(hub().is_running("living-room"));
  EXPECT_EQ(hub().claimed_by("relay_1"), "living-room");
}

static const char *const STUDIO =
    R"({"name":"Studio","enabled":false,"kind":"pid","sensor_id":"room","heat":{"relay_id":"relay_1"},)"
    R"("cool":{"relay_id":"relay_2"},"mode":"heat_cool","setpoint":22})";

TEST_F(Editor, TakingTwoRelaysOverNamesEveryThermostatItStopped) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  ASSERT_EQ(this->create(FLOOR), "floor");
  ASSERT_EQ(this->post("save", STUDIO).code, 200);

  Reply reply = this->post("enable?id=studio&value=true&take_over=true");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.message(), "Thermostat enabled; \"Living Room\" and \"Floor\" stopped");
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_FALSE(hub().is_running("floor"));
  EXPECT_FALSE(hub().store().get("floor")->enabled);
  EXPECT_EQ(hub().claimed_by("relay_1"), "studio");
  EXPECT_EQ(hub().claimed_by("relay_2"), "studio");
}

TEST_F(Editor, AThermostatHoldingBothRelaysIsNamedOnce) {
  std::string studio = STUDIO;
  studio.replace(studio.find(R"("enabled":false)"), 15, R"("enabled":true)");
  ASSERT_EQ(this->create(studio.c_str()), "studio");
  std::string attic = STUDIO;
  attic.replace(attic.find("Studio"), 6, "Attic");
  ASSERT_EQ(this->post("save", attic).code, 200);

  Reply reply = this->post("enable?id=attic&value=true&take_over=true");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.message(), "Thermostat enabled; \"Studio\" stopped");
  EXPECT_EQ(hub().claimed_by("relay_1"), "attic");
  EXPECT_EQ(hub().claimed_by("relay_2"), "attic");
}

// The thermostat starts or stops either way; `persisted` says whether that outlives a reboot.
TEST_F(Editor, EnableSaysWhenTheFlagDidNotReachTheFile) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  storage().set_base_path("/proc/definitely-not-writable");
  Reply stopped = this->post("enable?id=living-room&value=false");
  const bool stopped_running = hub().is_running("living-room");
  Reply started = this->post("enable?id=living-room&value=true");
  storage().set_base_path(this->base_path);

  EXPECT_EQ(stopped.body, R"({"success":true,"message":"Thermostat disabled","persisted":false})");
  EXPECT_FALSE(stopped_running);
  EXPECT_EQ(started.body, R"({"success":true,"message":"Thermostat enabled","persisted":false})");
  EXPECT_TRUE(hub().is_running("living-room"));
}

// A held relay is a 409 as on a Save, missing sensor or not; a take-over stops the holder only for
// a thermostat that runs in its place, so one whose sensor or relay is missing is a 400.
TEST_F(Editor, EnableNeedsTheSensorAndRelaysBeforeItTakesAnythingOver) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  std::string attic = with(LIVING_ROOM, R"("enabled":false)");
  attic.replace(attic.find("Living Room"), 11, "Attic");
  attic.replace(attic.find("\"room\""), 6, "\"attic\"");
  ASSERT_EQ(this->post("save", attic).code, 200);

  Reply held = this->post("enable?id=attic&value=true");
  EXPECT_EQ(held.code, 409);
  EXPECT_EQ(held.error(), "\"Relay 1\" is already driven by \"Living Room\"");
  Reply reply = this->post("enable?id=attic&value=true&take_over=true");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "No sensor \"attic\" on this device");

  std::string studio = STUDIO;
  studio.replace(studio.find("relay_2"), 7, "relay_9");
  ASSERT_EQ(this->post("save", studio).code, 200);
  reply = this->post("enable?id=studio&value=true&take_over=true");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "No switch \"relay_9\" on this device");

  EXPECT_TRUE(hub().is_running("living-room"));
  EXPECT_TRUE(hub().store().get("living-room")->enabled);
  EXPECT_FALSE(hub().store().get("attic")->enabled);
  EXPECT_FALSE(hub().store().get("studio")->enabled);
}

// As a Save: a start whose sensor or relay is not on the device stores the thermostat enabled,
// lets it wait, and answers with the same warning, in a key of its own and in the message.
TEST_F(Editor, EnableOfAThermostatWhoseSensorOrRelayIsMissingStoresItEnabledToWait) {
  std::string attic = with(LIVING_ROOM, R"("enabled":false)");
  attic.replace(attic.find("\"room\""), 6, "\"attic\"");
  ASSERT_EQ(this->post("save", attic).code, 200);

  Reply reply = this->post("enable?id=living-room&value=true");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat enabled; not started: sensor 'attic' not found",)"
                        R"("persisted":true,"warning":"not started: sensor 'attic' not found"})");
  EXPECT_TRUE(hub().store().get("living-room")->enabled);
  EXPECT_NE(this->file("living-room.json").find(R"("enabled":true)"), std::string::npos);
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_EQ(hub().claimed_by("relay_1"), "");
  EXPECT_EQ(this->get("list")["controllers"][0]["waiting"].as<std::string>(), "not started: sensor 'attic' not found");

  // Already stored enabled and waiting: the same answer.
  reply = this->post("enable?id=living-room&value=true");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply["warning"].as<std::string>(), "not started: sensor 'attic' not found");

  std::string no_relay = with(FLOOR, R"("enabled":false)");
  no_relay.replace(no_relay.find("relay_2"), 7, "relay_9");
  ASSERT_EQ(this->post("save", no_relay).code, 200);
  reply = this->post("enable?id=floor&value=true");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.message(), "Thermostat enabled; not started: relay 'relay_9' not found");
  EXPECT_EQ(reply["warning"].as<std::string>(), "not started: relay 'relay_9' not found");
  EXPECT_TRUE(hub().store().get("floor")->enabled);
  EXPECT_FALSE(hub().is_running("floor"));

  // A running thermostat's enable, and any stop, carry none.
  std::string den = LIVING_ROOM;
  den.replace(den.find("Living Room"), 11, "Den");
  ASSERT_EQ(this->create(den.c_str()), "den");
  EXPECT_TRUE(this->post("enable?id=den&value=true")["warning"].isUnbound());
  EXPECT_TRUE(this->post("enable?id=floor&value=false")["warning"].isUnbound());
}

// The 400 above is for a take-over that would stop a running thermostat: with none on the relay,
// take_over=true answers as a plain enable, and the thermostat waits.
TEST_F(Editor, ATakeOverWithNoHolderWaitsForAMissingSensor) {
  std::string attic = with(LIVING_ROOM, R"("enabled":false)");
  attic.replace(attic.find("\"room\""), 6, "\"attic\"");
  ASSERT_EQ(this->post("save", attic).code, 200);

  Reply reply = this->post("enable?id=living-room&value=true&take_over=true");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat enabled; not started: sensor 'attic' not found",)"
                        R"("persisted":true,"warning":"not started: sensor 'attic' not found"})");
  EXPECT_TRUE(hub().store().get("living-room")->enabled);
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_EQ(hub().claimed_by("relay_1"), "");
}

// What a Save refuses, an enable refuses too: a sensor that is there but not in °C.
TEST_F(Editor, EnableRefusesASensorNotInCelsius) {
  std::string uptime = with(LIVING_ROOM, R"("enabled":false)");
  uptime.replace(uptime.find("\"room\""), 6, "\"uptime\"");
  ASSERT_EQ(this->post("save", uptime).code, 200);
  Reply reply = this->post("enable?id=living-room&value=true");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "\"Uptime\" reports s, not °C");
  EXPECT_FALSE(hub().store().get("living-room")->enabled);
}

// --- setpoint ---

// The stand-in hands a query over undecoded (#90), so each value below is what the handler
// reads once the device's server has decoded it: a '+' in one was sent as %2B, since a bare
// '+' decodes to a space.

TEST_F(Editor, SetpointMovesTheTargetRunningOrNot) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  Reply reply = this->post("setpoint?id=living-room&value=23.5");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Setpoint updated"})");
  EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, 23.5f);

  ASSERT_EQ(this->post("enable?id=living-room&value=false").code, 200);
  EXPECT_EQ(this->post("setpoint?id=living-room&value=18").code, 200);
  EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, 18.f);

  // Held inside the visual range, 5 to 45 by default.
  EXPECT_EQ(this->post("setpoint?id=living-room&value=1e30").code, 200);
  EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, 45.f);
  EXPECT_EQ(this->post("setpoint?id=living-room&value=-1e300").code, 200);
  EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, 5.f);
}

// A number the whole way through: what strtod stops early on, or reads as nan or inf, is no
// target.
TEST_F(Editor, SetpointReadsItsValueStrictly) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  for (const char *bad :
       {"", "abc", "nan", "NaN", "inf", "-inf", "0x10", " 21", "21 ", "21abc", "1e", ".", "+", "1e400", "2,5"}) {
    Reply reply = this->post(std::string("setpoint?id=living-room&value=") + bad);
    EXPECT_EQ(reply.code, 400) << "'" << bad << "'";
    EXPECT_EQ(reply.error(), "Invalid value parameter") << "'" << bad << "'";
  }
  for (const auto &good :
       {std::pair<const char *, float>{"+21.5", 21.5f}, {"2.2e1", 22.f}, {"21.", 21.f}, {".5e2", 45.f}, {"-0", 5.f}}) {
    Reply reply = this->post(std::string("setpoint?id=living-room&value=") + good.first);
    EXPECT_EQ(reply.code, 200) << good.first;
    EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, good.second) << good.first;
  }
  Reply reply = this->post("setpoint?id=living-room");
  EXPECT_EQ(reply.error(), "Missing value parameter");
  reply = this->post("setpoint?id=ghost&value=abc");
  EXPECT_EQ(reply.error(), "Invalid value parameter");
  reply = this->post("setpoint?id=ghost&value=20");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Thermostat not found");
}

// Every way a person, or the client's String(value), may write a decimal.
TEST_F(Editor, SetpointTakesEveryDecimalNotation) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  for (const auto &good : {std::pair<const char *, float>{"2.2E1", 22.f},
                           {"2.2e+1", 22.f},
                           {"215e-1", 21.5f},
                           {"215E-01", 21.5f},
                           {"0021.50", 21.5f},
                           {"-0.0", 5.f},
                           // Past a float's range, and below its smallest step: both still numbers.
                           {"1e300", 45.f},
                           {"1e-400", 5.f}}) {
    Reply reply = this->post(std::string("setpoint?id=living-room&value=") + good.first);
    EXPECT_EQ(reply.code, 200) << good.first << ": " << reply.body;
    EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, good.second) << good.first;
  }
}

TEST_F(Editor, SetpointRefusesWhatOnlyStartsLikeANumber) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  for (const char *bad : {"1e+", "1e-", "e5", "E5", ".e1", "--1", "+-1", "1.2.3", "1e5.5", "0x1p3", "1_000", "infinity",
                          "-nan", "22\t"}) {
    Reply reply = this->post(std::string("setpoint?id=living-room&value=") + bad);
    EXPECT_EQ(reply.code, 400) << "'" << bad << "'";
    EXPECT_EQ(reply.error(), "Invalid value parameter") << "'" << bad << "'";
  }
  EXPECT_FLOAT_EQ(hub().store().get("living-room")->setpoint, 22.f);
}

// The id before the value, both before the loop task is asked for anything.
TEST_F(Editor, SetpointReadsItsIdFirst) {
  hub().jobs = 0;
  Reply reply = this->post("setpoint?value=20");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Missing id parameter");
  reply = this->post("setpoint?id=Living&value=abc");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Invalid id parameter");
  EXPECT_EQ(hub().jobs, 0);
}

// --- status ---

TEST_F(Editor, StatusReportsTheControlLoop) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  ASSERT_EQ(this->create(FLOOR), "floor");

  // Before a first reading: no temperature, no age, and the relays open. Waiting is no fault.
  hub().loop();
  Reply before = this->get("status?id=living-room");
  ASSERT_EQ(before.code, 200) << before.body;
  ASSERT_EQ(before["controllers"].size(), 1u);
  JsonObject pid = before["controllers"][0];
  EXPECT_TRUE(pid["current_temperature"].isNull());
  EXPECT_TRUE(pid["sensor_age_s"].isNull());
  EXPECT_EQ(pid["fault"].as<std::string>(), "none");
  EXPECT_EQ(pid["action"].as<std::string>(), "idle");
  EXPECT_FALSE(pid["heat_relay_on"].as<bool>());

  entities().room.publish_state(18.f);
  entities().floor.publish_state(26.f);
  // Ten seconds on, so the reading has an age.
  hub().ms += 10000;
  hub().loop();

  Reply reply = this->get("status");
  ASSERT_EQ(reply.code, 200) << reply.body;
  ASSERT_EQ(reply["controllers"].size(), 2u);
  // Sorted by id, as list is.
  JsonObject floor = reply["controllers"][0];
  JsonObject room = reply["controllers"][1];

  EXPECT_EQ(room["id"].as<std::string>(), "living-room");
  EXPECT_TRUE(room["running"].as<bool>());
  EXPECT_EQ(room["waiting"].as<std::string>(), "");
  EXPECT_TRUE(room["waiting"].is<const char *>()) << "there, and empty";
  EXPECT_EQ(room["action"].as<std::string>(), "heating");
  EXPECT_EQ(room["fault"].as<std::string>(), "none");
  EXPECT_FLOAT_EQ(room["current_temperature"].as<float>(), 18.f);
  EXPECT_FLOAT_EQ(room["sensor_age_s"].as<float>(), 10.f);
  EXPECT_FLOAT_EQ(room["setpoint"].as<float>(), 22.f);
  EXPECT_FLOAT_EQ(room["min_temperature"].as<float>(), 5.f);
  EXPECT_FLOAT_EQ(room["max_temperature"].as<float>(), 45.f);
  EXPECT_FLOAT_EQ(room["step"].as<float>(), 0.5f);
  EXPECT_TRUE(room["switch_low"].isUnbound()) << "a PID has no switching points";
  EXPECT_GT(room["heat_duty"].as<float>(), 0.f);
  EXPECT_FLOAT_EQ(room["cool_duty"].as<float>(), 0.f);
  EXPECT_TRUE(room["heat_relay_on"].as<bool>());
  EXPECT_FALSE(room["cool_relay_on"].as<bool>());
  EXPECT_FLOAT_EQ(room["pid"]["error"].as<float>(), 4.f);
  EXPECT_FALSE(room["pid"]["proportional"].isNull());
  EXPECT_FALSE(room["pid"]["in_deadband"].as<bool>());

  EXPECT_EQ(floor["id"].as<std::string>(), "floor");
  EXPECT_EQ(floor["action"].as<std::string>(), "idle");
  EXPECT_FLOAT_EQ(floor["switch_low"].as<float>(), 23.5f);
  EXPECT_FLOAT_EQ(floor["switch_high"].as<float>(), 24.5f);
  EXPECT_TRUE(floor["pid"].isUnbound()) << "only a running PID has terms";
}

TEST_F(Editor, StatusReportsACoolingThermostat) {
  ASSERT_EQ(this->create(R"({"name":"Cellar","kind":"pid","sensor_id":"floor","cool":{"relay_id":"relay_2"},)"
                         R"("mode":"cool","setpoint":12})"),
            "cellar");
  hub().loop();
  entities().floor.publish_state(20.f);
  hub().loop();

  Reply reply = this->get("status?id=cellar");
  ASSERT_EQ(reply.code, 200) << reply.body;
  JsonObject row = reply["controllers"][0];
  EXPECT_EQ(row["action"].as<std::string>(), "cooling");
  EXPECT_FLOAT_EQ(row["heat_duty"].as<float>(), 0.f);
  EXPECT_GT(row["cool_duty"].as<float>(), 0.f);
  EXPECT_FALSE(row["heat_relay_on"].as<bool>());
  EXPECT_TRUE(row["cool_relay_on"].as<bool>());
  EXPECT_FLOAT_EQ(row["pid"]["error"].as<float>(), -8.f);
  EXPECT_TRUE(entities().relay2.state);
}

// A stopped thermostat still shows its room, read off the sensor itself.
TEST_F(Editor, StatusOfAStoppedThermostat) {
  ASSERT_EQ(this->create(with(LIVING_ROOM, R"("enabled":false)").c_str()), "living-room");
  entities().room.publish_state(19.25f);
  Reply reply = this->get("status");
  ASSERT_EQ(reply["controllers"].size(), 1u);
  JsonObject row = reply["controllers"][0];
  EXPECT_FALSE(row["running"].as<bool>());
  EXPECT_EQ(row["waiting"].as<std::string>(), "") << "a disabled thermostat waits for nothing";
  EXPECT_EQ(row["action"].as<std::string>(), "off");
  EXPECT_EQ(row["fault"].as<std::string>(), "none");
  EXPECT_FLOAT_EQ(row["current_temperature"].as<float>(), 19.25f);
  EXPECT_TRUE(row["sensor_age_s"].isNull());
  EXPECT_FLOAT_EQ(row["heat_duty"].as<float>(), 0.f);
  EXPECT_FALSE(row["heat_relay_on"].as<bool>());
  EXPECT_TRUE(row["pid"].isUnbound());
}

TEST_F(Editor, StatusOfOneNeedsAnIdThatIsThere) {
  EXPECT_EQ(this->get("status?id=ghost").code, 404);
  EXPECT_EQ(this->get("status?id=ghost").error(), "Thermostat not found");
  EXPECT_EQ(this->get("status?id=").error(), "Invalid id parameter");
  EXPECT_EQ(this->get("status").body, R"({"success":true,"controllers":[]})");
}

// Both say why an enabled thermostat does not run, long after the answer that said it first; a
// running or a disabled one carries the key empty.
TEST_F(Editor, ListAndStatusSayWhyEachEnabledThermostatWaits) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  std::string attic = FLOOR;
  attic.replace(attic.find("\"floor\""), 7, "\"attic\"");
  ASSERT_EQ(this->create(attic.c_str()), "floor");
  std::string guest = with(LIVING_ROOM, R"("enabled":false)");
  guest.replace(guest.find("Living Room"), 11, "Guest Room");
  ASSERT_EQ(this->create(guest.c_str()), "guest-room");
  hub().loop();

  for (const char *route : {"list", "status"}) {
    Reply reply = this->get(route);
    ASSERT_EQ(reply["controllers"].size(), 3u) << route;
    EXPECT_EQ(reply["controllers"][0]["id"].as<std::string>(), "floor") << route;
    EXPECT_EQ(reply["controllers"][0]["waiting"].as<std::string>(), "not started: sensor 'attic' not found") << route;
    EXPECT_EQ(reply["controllers"][1]["waiting"].as<std::string>(), "") << route;
    EXPECT_EQ(reply["controllers"][2]["waiting"].as<std::string>(), "") << route;
  }
  EXPECT_EQ(this->get("status?id=floor")["controllers"][0]["waiting"].as<std::string>(),
            "not started: sensor 'attic' not found");
}

// --- entities, schema ---

// Only the sensors a thermostat may run on: visible and in °C, so neither Uptime nor Counter.
TEST_F(Editor, EntitiesListsThermostatInputsAndWhoHoldsEachRelay) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  Reply reply = this->get("entities");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, R"({"success":true,)"
                        R"("sensors":[{"object_id":"room","name":"Room","unit":"°C"},)"
                        R"({"object_id":"floor","name":"Floor","unit":"°C"},)"
                        R"({"object_id":"temp_1","name":"Temp 1","unit":"°C"},)"
                        R"({"object_id":"temp_2","name":"Temp 2","unit":"°C"}],)"
                        R"("switches":[{"object_id":"relay_1","name":"Relay 1","claimed_by":"living-room"},)"
                        R"({"object_id":"relay_2","name":"Relay 2","claimed_by":""}]})");
}

// Straight from the table the codec clamps against, so the form cannot offer what the device
// would then change.
TEST_F(Editor, SchemaIsTheParameterTable) {
  Reply reply = this->get("schema");
  ASSERT_EQ(reply.code, 200);
  std::string words;
  serializeJson(reply["kinds"], words);
  EXPECT_EQ(words, R"(["pid","bang_bang"])");
  words.clear();
  serializeJson(reply["modes"], words);
  EXPECT_EQ(words, R"(["off","heat","cool","heat_cool"])");
  words.clear();
  serializeJson(reply["faults"], words);
  EXPECT_EQ(words, R"(["none","sensor_stale","overtemp"])");
  EXPECT_EQ(reply["max_controllers"].as<int>(), 3);
  EXPECT_EQ(reply["name_max_length"].as<int>(), 48);

  size_t seen = 0;
  for (JsonPair group : reply["params"].as<JsonObject>()) {
    for (JsonObject entry : group.value().as<JsonArray>()) {
      const std::string key = entry["key"];
      const climate_hub::ParamDesc *param = climate_hub::find_param(key.c_str());
      ASSERT_NE(param, nullptr) << key;
      EXPECT_EQ(std::string(group.key().c_str()), param->group) << key;
      EXPECT_EQ(entry["group"].as<std::string>(), param->group) << key;
      EXPECT_EQ(entry["label"].as<std::string>(), param->label) << key;
      EXPECT_EQ(entry["kind"].as<std::string>(), param->kind) << key;
      EXPECT_FLOAT_EQ(entry["def"].as<float>(), param->def) << key;
      EXPECT_FLOAT_EQ(entry["min"].as<float>(), param->min) << key;
      EXPECT_FLOAT_EQ(entry["max"].as<float>(), param->max) << key;
      EXPECT_EQ(entry["integer"].as<bool>(), param->integer) << key;
      EXPECT_FALSE(entry["hint"].as<std::string>().empty()) << key;
      seen++;
    }
  }
  EXPECT_EQ(seen, climate_hub::PARAM_COUNT);
  EXPECT_FLOAT_EQ(reply["params"]["output"][1]["def"].as<float>(), 10.f) << "min_on_s";
}

}  // namespace esphome::web_climate_editor::testing
