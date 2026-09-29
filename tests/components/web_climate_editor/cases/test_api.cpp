#include "common.h"
#include "esphome/components/climate_hub/param_table.h"

// Every route's answer, and its refusals in the order client/mock/climateMock.ts gives them:
// the dashboard is built against that mock, so a refusal that comes out differently here is a
// different message on the device than in development.
namespace esphome::web_climate_editor::testing {

static std::string with(const char *json, const std::string &fields) {
  std::string out = json;
  out.insert(1, fields + ",");
  return out;
}

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
                       R"("sensor_id":"room","heat_relay_id":"relay_1","cool_relay_id":"","running":true}]})");

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

// Running is what a document that says enabled asks for: it is saved only if it can.
TEST_F(Editor, AnEnabledSaveNeedsItsSensorAndRelaysFree) {
  std::string no_sensor = LIVING_ROOM;
  no_sensor.replace(no_sensor.find("\"room\""), 6, "\"attic\"");
  Reply reply = this->post("save", no_sensor);
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "No sensor \"attic\" on this device");

  std::string no_relay = LIVING_ROOM;
  no_relay.replace(no_relay.find("relay_1"), 7, "relay_9");
  reply = this->post("save", no_relay);
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "No switch \"relay_9\" on this device");
  EXPECT_TRUE(this->files().empty());

  // A disabled one may wait for what it names.
  EXPECT_EQ(this->post("save", with(no_sensor.c_str(), R"("enabled":false)")).code, 200);

  ASSERT_EQ(this->create(FLOOR), "floor");
  std::string shares = LIVING_ROOM;
  shares.replace(shares.find("Living Room"), 11, "Kitchen");
  shares.replace(shares.find("relay_1"), 7, "relay_2");
  reply = this->post("save", shares);
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "\"Relay 2\" is already driven by \"Floor\"");
}

TEST_F(Editor, AFileThatCannotBeWrittenIsAServerError) {
  storage().set_base_path("/proc/definitely-not-writable");
  Reply reply = this->post("save", LIVING_ROOM);
  storage().set_base_path(this->base_path);
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "The thermostat's file could not be written");
  EXPECT_EQ(hub().store().size(), 0u);
}

// --- delete ---

TEST_F(Editor, DeleteRemovesTheThermostatAndItsFile) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  Reply reply = this->post("delete?id=living-room");
  EXPECT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Thermostat deleted"})");
  EXPECT_TRUE(this->files().empty());
  EXPECT_FALSE(hub().is_running("living-room"));
  EXPECT_EQ(hub().claimed_by("relay_1"), "");

  reply = this->post("delete?id=living-room");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Thermostat not found");
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

TEST_F(Editor, EnableNeedsTheSensorBeforeItTakesAnythingOver) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  std::string attic = with(LIVING_ROOM, R"("enabled":false)");
  attic.replace(attic.find("Living Room"), 11, "Attic");
  attic.replace(attic.find("\"room\""), 6, "\"attic\"");
  ASSERT_EQ(this->post("save", attic).code, 200);

  Reply reply = this->post("enable?id=attic&value=true&take_over=true");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "No sensor \"attic\" on this device");
  EXPECT_TRUE(hub().is_running("living-room"));
}

// --- setpoint ---

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

// --- status ---

TEST_F(Editor, StatusReportsTheControlLoop) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  ASSERT_EQ(this->create(FLOOR), "floor");

  // Before a first reading: no temperature, no age, and the relays open.
  hub().loop();
  Reply before = this->get("status?id=living-room");
  ASSERT_EQ(before.code, 200) << before.body;
  ASSERT_EQ(before["controllers"].size(), 1u);
  JsonObject pid = before["controllers"][0];
  EXPECT_TRUE(pid["current_temperature"].isNull());
  EXPECT_TRUE(pid["sensor_age_s"].isNull());
  EXPECT_EQ(pid["fault"].as<std::string>(), "sensor_stale");

  entities().room.publish_state(18.f);
  entities().floor.publish_state(26.f);
  // Past the 10 s the relay stays open after the stale pass opened it.
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

// A stopped thermostat still shows its room, read off the sensor itself.
TEST_F(Editor, StatusOfAStoppedThermostat) {
  ASSERT_EQ(this->create(with(LIVING_ROOM, R"("enabled":false)").c_str()), "living-room");
  entities().room.publish_state(19.25f);
  Reply reply = this->get("status");
  ASSERT_EQ(reply["controllers"].size(), 1u);
  JsonObject row = reply["controllers"][0];
  EXPECT_FALSE(row["running"].as<bool>());
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

// --- entities, schema ---

TEST_F(Editor, EntitiesListsWhatIsNotInternalAndWhoHoldsEachRelay) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  Reply reply = this->get("entities");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, R"({"success":true,)"
                        R"("sensors":[{"object_id":"room","name":"Room","unit":"°C"},)"
                        R"({"object_id":"floor","name":"Floor","unit":"°C"}],)"
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
  EXPECT_EQ(words, R"(["none","sensor_missing","sensor_stale","relay_missing","overtemp"])");
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
