#include "common.h"

// The saved gain sets over HTTP where only the device can show it: what a listing reads where, a
// thousand sets in pages, a calibration's set in /status, the device's clock in a name, and the
// refusals that need a state the client mock has no way into. The requests the mock answers too
// are in ../contract.json.
namespace esphome::web_climate_editor::testing {
namespace {

// 2026-11-06 14:05 UTC, which the device's clock gives as local time here.
constexpr time_t SAVED_AT = 1793973900;

// One reading a minute after a loop pass, as a probe gives them.
void reading(float value) {
  hub().ms += 60000;
  hub().loop();
  entities().room.publish_state(value);
  hub().loop();
}

}  // namespace

class GainSetRoutes : public Editor {
 protected:
  // Living Room's target is 22: six readings across the band finish a calibration.
  void calibrate() {
    reading(22.f);
    ASSERT_EQ(this->post("autotune?id=living-room&value=true").code, 200);
    for (float value : {21.7f, 22.3f, 21.6f, 22.4f, 21.7f, 22.3f})
      reading(value);
  }

  void SetUp() override {
    Editor::SetUp();
    ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  }

  std::vector<uint32_t> numbers(const std::string &query) {
    Reply reply = this->get("gains?id=living-room" + query);
    EXPECT_EQ(reply.code, 200) << reply.body;
    std::vector<uint32_t> out;
    for (JsonVariant set : reply["sets"].as<JsonArray>())
      out.push_back(set["n"].as<uint32_t>());
    return out;
  }
};

// One job opens the file on the loop task; the sets are read and sent on the server's.
TEST_F(GainSetRoutes, AListingReadsTheFileOnTheServersTask) {
  for (int i = 0; i < 3; i++)
    ASSERT_EQ(this->post("gains/add?id=living-room").code, 200);
  hub().jobs = 0;
  Reply reply = this->get("gains?id=living-room");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(hub().jobs, 1);
  EXPECT_EQ(reply.type, "application/json");
  EXPECT_EQ(reply["sets"].size(), 3u);
  EXPECT_EQ(hub().open_readers(), 0u) << "closed when the answer is out";
  EXPECT_EQ(reply.body.substr(0, 25), R"({"success":true,"sets":[{)");
  EXPECT_EQ(reply.body.substr(reply.body.size() - 16), R"(}],"more":false})");
}

// No limit on the sets a thermostat keeps: a listing answers 200 of them at most, oldest first, and
// the next page starts after the last one it had. Lines that are no set are passed over.
TEST_F(GainSetRoutes, AThousandSetsComeInPages) {
  mkdir(this->gains().c_str(), 0755);
  std::ofstream out(this->sets_of("living-room"));
  for (uint32_t n = 1; n <= 1000; n++) {
    out << R"({"n":)" << n << R"(,"name":"Set )" << n
        << R"(","direction":"heat","source":"manual","kp":1,"ki":0,"kd":0})"
        << "\n";
    if (n % 100 == 0)
      out << "not a set\n";
  }
  out.close();

  std::vector<uint32_t> seen;
  uint32_t after = 0;
  int pages = 0;
  for (;;) {
    Reply page = this->get("gains?id=living-room&after=" + std::to_string(after));
    ASSERT_EQ(page.code, 200);
    pages++;
    const size_t count = page["sets"].size();
    ASSERT_LE(count, 200u);
    for (JsonVariant set : page["sets"].as<JsonArray>())
      seen.push_back(set["n"].as<uint32_t>());
    if (!page["more"].as<bool>())
      break;
    EXPECT_EQ(count, 200u);
    after = seen.back();
  }
  EXPECT_EQ(pages, 5);
  ASSERT_EQ(seen.size(), 1000u);
  for (uint32_t n = 1; n <= 1000; n++)
    EXPECT_EQ(seen[n - 1], n);
  EXPECT_EQ(this->numbers("&after=990&limit=5"), (std::vector<uint32_t>{991, 992, 993, 994, 995}));
  EXPECT_EQ(this->post("gains/add?id=living-room")["set"]["n"], 1001);
}

TEST_F(GainSetRoutes, EveryRouteIsBusyWhenTheLoopIsAndChangesNothing) {
  ASSERT_EQ(this->post("gains/add?id=living-room").code, 200);
  hub().loop_busy = true;
  for (const char *route :
       {"gains/add?id=living-room", "gains/rename?id=living-room&set=1", "gains/delete?id=living-room&set=1",
        "gains/apply?id=living-room&set=1", "gains/derive?id=living-room&set=1&rule=zn_pid"}) {
    Reply reply = this->post(route, R"({"name":"x"})");
    EXPECT_EQ(reply.code, 503) << route;
    EXPECT_EQ(reply.error(), "Device busy") << route;
  }
  EXPECT_EQ(this->get("gains?id=living-room").code, 503);
  hub().loop_busy = false;
  EXPECT_EQ(this->numbers(""), std::vector<uint32_t>{1});
  EXPECT_EQ(this->get("get?id=living-room")["revision"], 0);
}

// A reader open elsewhere than a listing of this server's, as a lambda's: a rewrite waits for it.
TEST_F(GainSetRoutes, ARewriteWhileTheFileIsReadIsBusy) {
  ASSERT_EQ(this->post("gains/add?id=living-room").code, 200);
  climate_hub::Result result;
  auto reader = hub().open_gain_sets("living-room", &result);
  Reply rename = this->post("gains/rename?id=living-room&set=1", R"({"name":"x"})");
  EXPECT_EQ(rename.code, 503);
  EXPECT_EQ(rename.error(), "The gain sets are being read; try again");
  EXPECT_EQ(this->post("gains/delete?id=living-room&set=1").code, 503);
  Reply added = this->post("gains/add?id=living-room");
  EXPECT_EQ(added.code, 200) << "an add waits in RAM";
  EXPECT_EQ(added["set"]["n"], 2);
  reader.reset();
  EXPECT_EQ(this->numbers(""), (std::vector<uint32_t>{1, 2}));
}

TEST_F(GainSetRoutes, ABodyOver8KiBIs413) {
  const std::string big = R"({"name":")" + std::string(9000, 'x') + R"("})";
  for (const char *route : {"gains/add?id=living-room", "gains/rename?id=living-room&set=1",
                            "gains/derive?id=living-room&set=1&rule=zn_pi"}) {
    Reply reply = this->post(route, big);
    EXPECT_EQ(reply.code, 413) << route;
    EXPECT_EQ(reply.error(), "Request body over 8 KiB") << route;
  }
  Reply deep = this->post("gains/add?id=living-room", R"({"name":{"a":{"b":1}}})");
  EXPECT_EQ(deep.code, 400);
  EXPECT_EQ(deep.error(), "JSON parse error: TooDeep");
  Reply form =
      this->call(HTTP_POST, "gains/rename?id=living-room&set=1", "name=x", "application/x-www-form-urlencoded");
  EXPECT_EQ(form.code, 400);
  EXPECT_EQ(form.error(), "Empty request body");
  EXPECT_TRUE(this->list(this->gains()).empty());
}

// With the clock set, a set given no name is named for the local time it was saved at, and dated.
TEST_F(GainSetRoutes, TheClockNamesAndDatesASet) {
  hub().clock = ESPTime::from_epoch_utc(SAVED_AT);
  Reply added = this->post("gains/add?id=living-room");
  ASSERT_EQ(added.code, 200);
  EXPECT_EQ(added["set"]["name"], "2026-11-06 14:05 manual");
  EXPECT_EQ(added["set"]["time"].as<int64_t>(), SAVED_AT);
  Reply listed = this->get("gains?id=living-room");
  EXPECT_EQ(listed["sets"][0]["time"].as<int64_t>(), SAVED_AT);
}

// The status names the set a calibration's result went into, as it is named now.
TEST_F(GainSetRoutes, TheStatusNamesTheSetACalibrationSaved) {
  this->calibrate();
  Reply status = this->get("status?id=living-room");
  JsonVariant run = status["controllers"][0]["autotune"];
  ASSERT_EQ(run["state"], "succeeded") << status.body;
  EXPECT_EQ(run["set"]["n"], 1);
  EXPECT_EQ(run["set"]["name"], "Heat ZN PI #1");
  EXPECT_EQ(run["set"].size(), 2u);

  Reply listed = this->get("gains?id=living-room");
  JsonVariant set = listed["sets"][0];
  EXPECT_EQ(set["source"], "calibration");
  EXPECT_EQ(set["rule"], "zn_pi");
  EXPECT_FLOAT_EQ(set["ku"].as<float>(), run["ku"].as<float>());
  EXPECT_FLOAT_EQ(set["pu"].as<float>(), run["pu"].as<float>());
  EXPECT_FLOAT_EQ(set["kp"].as<float>(), run["new"]["kp"].as<float>());
  EXPECT_EQ(set["flags"].size(), run["flags"].size());

  ASSERT_EQ(this->post("gains/rename?id=living-room&set=1", R"({"name":"Radiators"})").code, 200);
  EXPECT_EQ(this->get("status?id=living-room")["controllers"][0]["autotune"]["set"]["name"], "Radiators");
  ASSERT_EQ(this->post("gains/delete?id=living-room&set=1").code, 200);
  EXPECT_TRUE(this->get("status?id=living-room")["controllers"][0]["autotune"]["set"].isNull());

  // Another rule for the same measurement, applied: the form read before it is stale.
  this->calibrate();
  Reply derived = this->post("gains/derive?id=living-room&set=2&rule=zn_pid&apply=true");
  ASSERT_EQ(derived.code, 200) << derived.body;
  EXPECT_EQ(derived["set"]["from"], 2);
  EXPECT_EQ(derived["revision"], 3) << "two calibrations and an apply";
  EXPECT_FLOAT_EQ(this->get("get?id=living-room")["pid"]["heat"]["kp"].as<float>(), derived["set"]["kp"].as<float>());
}

TEST_F(GainSetRoutes, AnApplyTheFileDidNotTakeSaysSo) {
  ASSERT_EQ(this->post("gains/add?id=living-room").code, 200);
  hub().max_file_bytes = 100;
  Reply applied = this->post("gains/apply?id=living-room&set=1");
  ASSERT_EQ(applied.code, 200);
  EXPECT_EQ(applied["persisted"], false);
  EXPECT_EQ(applied["revision"], 1);
  hub().max_file_bytes = climate_hub::CONFIG_MAX_BYTES;
}

// A newer firmware's file is never written: its sets may be saved, renamed and deleted, but none
// goes into it.
TEST_F(GainSetRoutes, ANewerFirmwaresThermostatTakesNoSet) {
  ASSERT_EQ(this->post("gains/add?id=living-room").code, 200);
  JsonDocument file;
  ASSERT_EQ(deserializeJson(file, this->file("living-room.json")), DeserializationError::Ok);
  file["version"] = 9;
  std::string text;
  serializeJson(file, text);
  std::ofstream(this->folder() + "/living-room.json") << text;
  hub().reset();
  hub().setup();
  const char *const newer = "A newer firmware wrote this thermostat; update the firmware to change it";
  EXPECT_EQ(this->post("gains/apply?id=living-room&set=1").error(), newer);
  EXPECT_EQ(this->post("gains/add?id=living-room").code, 200);
  EXPECT_EQ(this->post("gains/rename?id=living-room&set=2", R"({"name":"x"})").code, 200);
  EXPECT_EQ(this->post("gains/delete?id=living-room&set=2").code, 200);
  EXPECT_EQ(this->numbers(""), std::vector<uint32_t>{1});
}

}  // namespace esphome::web_climate_editor::testing
