#include <cmath>
#include "common.h"

// A calibration over HTTP where only the device can show it: a run fed readings to its end, its
// numbers in /status against the hub's, the gains in /get, and the revision a Save is checked
// against. The requests the client mock answers too are in ../contract.json.
namespace esphome::web_climate_editor::testing {
namespace {

// One reading a minute after a loop pass, as a probe gives them.
void reading(float value) {
  hub().ms += 60000;
  hub().loop();
  entities().room.publish_state(value);
  hub().loop();
}

// Living Room's target is 22: six readings across the band, one switch each, finish a run.
void swing() {
  for (float value : {21.7f, 22.3f, 21.6f, 22.4f, 21.7f, 22.3f})
    reading(value);
}

JsonVariant autotune_of(Reply &reply) { return reply.json["controllers"][0]["autotune"]; }

}  // namespace

TEST_F(Editor, ARunShowsItsProgressInStatus) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  reading(22.f);
  Reply none = this->get("status?id=living-room");
  EXPECT_FALSE(none.json["controllers"][0]["autotune"].is<JsonObject>()) << "no run since boot";

  Reply started = this->post("autotune?id=living-room&value=true");
  ASSERT_EQ(started.code, 200) << started.body;
  EXPECT_EQ(started.message(), "Calibration started");
  reading(21.7f);
  reading(22.1f);

  Reply status = this->get("status?id=living-room");
  JsonVariant run = autotune_of(status);
  EXPECT_EQ(run["state"], "running");
  EXPECT_EQ(run["reason"], "");
  EXPECT_EQ(run["direction"], "heat");
  EXPECT_EQ(run["rule"], "zn_pi");
  EXPECT_EQ(run["phase"], "on") << "under the band at 21.7, not over it since";
  EXPECT_FLOAT_EQ(run["aim"].as<float>(), 22.25f) << "it heats until the room is 0.25 over";
  EXPECT_EQ(run["swings"], 1);
  EXPECT_EQ(run["elapsed_s"], 120);
  ASSERT_EQ(run["extremes"].size(), 1u) << "the first phase's, at the start";
  EXPECT_EQ(run["extremes"][0]["at_s"], 0);
  EXPECT_FLOAT_EQ(run["extremes"][0]["temperature"].as<float>(), 22.f);
  EXPECT_TRUE(run["ku"].isNull());
  EXPECT_TRUE(run["pu"].isNull());
  EXPECT_EQ(run["flags"].size(), 0u);
  EXPECT_FLOAT_EQ(run["old"]["kp"].as<float>(), 0.6f);
  EXPECT_FLOAT_EQ(run["old"]["ki"].as<float>(), 0.0025f);
  EXPECT_TRUE(run["new"].isNull());
  EXPECT_EQ(run["persisted"], true);
  EXPECT_EQ(status.json["controllers"][0]["action"], "heating") << "Home Assistant sees heating or idle";
}

// Started before a 32-bit millis() would wrap, a run times itself and its chart from its start
// after the wrap too.
TEST_F(Editor, ARunAcrossTheMillisWrapCountsFromItsStart) {
  hub().ms = (1ull << 32) - 150000;
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  reading(22.f);
  ASSERT_EQ(this->post("autotune?id=living-room&value=true").code, 200);
  for (float value : {21.7f, 22.3f, 21.6f})
    reading(value);
  ASSERT_GT(hub().ms, 1ull << 32);

  Reply status = this->get("status?id=living-room");
  JsonVariant run = autotune_of(status);
  ASSERT_EQ(run["state"], "running");
  EXPECT_EQ(run["elapsed_s"], 180);
  ASSERT_EQ(run["extremes"].size(), 3u);
  EXPECT_EQ(run["extremes"][0]["at_s"], 0);
  EXPECT_EQ(run["extremes"][1]["at_s"], 60) << "before the wrap";
  EXPECT_EQ(run["extremes"][2]["at_s"], 120) << "after it";
}

TEST_F(Editor, AFinishedRunShowsWhatItFoundAndGetHasIt) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  reading(22.f);
  ASSERT_EQ(this->post("autotune?id=living-room&value=true&rule=pessen").code, 200);
  swing();

  const climate_hub::AutotuneRun *found = hub().autotune("living-room");
  ASSERT_NE(found, nullptr);
  ASSERT_EQ(found->state(), climate_hub::AutotuneState::SUCCEEDED);
  Reply status = this->get("status?id=living-room");
  JsonVariant run = autotune_of(status);
  EXPECT_EQ(run["state"], "succeeded");
  EXPECT_EQ(run["rule"], "pessen");
  EXPECT_TRUE(run["phase"].isNull());
  EXPECT_TRUE(run["aim"].isNull());
  EXPECT_EQ(run["swings"], 6);
  EXPECT_EQ(run["elapsed_s"], 360);
  EXPECT_EQ(run["extremes"].size(), 6u);
  EXPECT_NEAR(run["ku"].as<float>(), found->tuner().ku(), 1e-5f);
  EXPECT_FLOAT_EQ(run["pu"].as<float>(), 120.f) << "a minute between crossings";
  EXPECT_FLOAT_EQ(run["new"]["kp"].as<float>(), found->new_gains().kp);
  EXPECT_FLOAT_EQ(run["new"]["ki"].as<float>(), found->new_gains().ki);
  EXPECT_FLOAT_EQ(run["new"]["kd"].as<float>(), found->new_gains().kd);
  EXPECT_FLOAT_EQ(run["old"]["kp"].as<float>(), 0.6f);

  Reply doc = this->get("get?id=living-room");
  EXPECT_EQ(doc["revision"], 1);
  EXPECT_FLOAT_EQ(doc["pid"]["kp"].as<float>(), found->new_gains().kp);
  EXPECT_FLOAT_EQ(doc["pid"]["kd"].as<float>(), found->new_gains().kd);
}

// The chart is the run's: a target that ended it, or one set after it succeeded, leaves it alone.
TEST_F(Editor, ALaterTargetLeavesTheRunsChartWhereItWas) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  reading(22.f);
  ASSERT_EQ(this->post("autotune?id=living-room&value=true").code, 200);
  reading(21.7f);
  reading(22.3f);
  ASSERT_EQ(this->post("setpoint?id=living-room&value=25").code, 200);
  Reply ended = this->get("status?id=living-room");
  JsonVariant run = autotune_of(ended);
  ASSERT_EQ(run["reason"], "target_changed");
  EXPECT_FLOAT_EQ(run["setpoint"].as<float>(), 22.f);
  ASSERT_EQ(run["extremes"].size(), 2u);
  EXPECT_FLOAT_EQ(run["extremes"][0]["temperature"].as<float>(), 22.f);
  EXPECT_FLOAT_EQ(run["extremes"][1]["temperature"].as<float>(), 21.7f);

  ASSERT_EQ(this->post("setpoint?id=living-room&value=22").code, 200);
  reading(22.f);
  ASSERT_EQ(this->post("autotune?id=living-room&value=true").code, 200);
  swing();
  Reply before = this->get("status?id=living-room");
  ASSERT_EQ(autotune_of(before)["state"], "succeeded");
  ASSERT_EQ(this->post("setpoint?id=living-room&value=19").code, 200);
  Reply after = this->get("status?id=living-room");
  EXPECT_FLOAT_EQ(autotune_of(after)["setpoint"].as<float>(), 22.f);
  std::string was;
  std::string is;
  serializeJson(autotune_of(before)["extremes"], was);
  serializeJson(autotune_of(after)["extremes"], is);
  EXPECT_EQ(was, is);
}

TEST_F(Editor, AFlaggedRunListsItsFlags) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  reading(22.f);
  ASSERT_EQ(this->post("autotune?id=living-room&value=true").code, 200);
  // A long phase among short ones, and one swing much larger than the rest.
  for (float value : {21.7f, 22.3f, 21.7f})
    reading(value);
  for (int minute = 0; minute < 60; minute++)
    reading(22.f);
  for (float value : {23.5f, 21.7f, 22.3f})
    reading(value);
  Reply status = this->get("status?id=living-room");
  JsonVariant flags = autotune_of(status)["flags"];
  ASSERT_EQ(flags.size(), 2u) << status.body;
  EXPECT_EQ(flags[0], "asymmetric");
  EXPECT_EQ(flags[1], "uneven");
}

// Readings with no time between them make a period of nothing, and an integral gain past its range.
TEST_F(Editor, AClampedGainIsFlagged) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  reading(22.f);
  ASSERT_EQ(this->post("autotune?id=living-room&value=true").code, 200);
  for (float value : {21.7f, 22.3f, 21.6f, 22.4f, 21.7f, 22.3f})
    entities().room.publish_state(value);
  Reply status = this->get("status?id=living-room");
  JsonVariant run = autotune_of(status);
  EXPECT_EQ(run["state"], "succeeded");
  EXPECT_EQ(run["pu"], 0);
  // Upstream's symmetry check takes half-periods of nothing for uneven ones.
  ASSERT_EQ(run["flags"].size(), 2u) << status.body;
  EXPECT_EQ(run["flags"][0], "asymmetric");
  EXPECT_EQ(run["flags"][1], "clamped");
  EXPECT_EQ(run["new"]["ki"], 1000) << "the top of its range";
}

// The Save that a form read before the run would send back is refused; the read after goes through.
TEST_F(Editor, ASaveOfAFormReadBeforeTheRunIsRefused) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  reading(22.f);
  Reply before = this->get("get?id=living-room");
  ASSERT_EQ(before["revision"], 0);
  ASSERT_EQ(this->post("autotune?id=living-room&value=true").code, 200);
  swing();

  std::string body;
  serializeJson(before.json, body);
  Reply stale = this->post("save", body);
  EXPECT_EQ(stale.code, 409);
  EXPECT_EQ(stale.error(), "The device changed this thermostat since it was read; reload it");
  EXPECT_NE(hub().store().get("living-room")->pid.kp, 0.6f) << "the tuned gains stay";

  Reply after = this->get("get?id=living-room");
  after.json["name"] = "Lounge";
  body.clear();
  serializeJson(after.json, body);
  Reply saved = this->post("save", body);
  EXPECT_EQ(saved.code, 200) << saved.body;
  EXPECT_EQ(this->get("get?id=living-room")["revision"], 1) << "a Save does not move it";
}

TEST_F(Editor, CancelEndsTheRunAndStatusSaysWhy) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  reading(22.f);
  ASSERT_EQ(this->post("autotune?id=living-room&value=true").code, 200);
  Reply cancelled = this->post("autotune?id=living-room&value=false");
  ASSERT_EQ(cancelled.code, 200) << cancelled.body;
  EXPECT_EQ(cancelled.message(), "Calibration cancelled");
  Reply status = this->get("status?id=living-room");
  EXPECT_EQ(autotune_of(status)["state"], "failed");
  EXPECT_EQ(autotune_of(status)["reason"], "cancelled");

  // Disabled, it keeps the last run's word.
  ASSERT_EQ(this->post("enable?id=living-room&value=false").code, 200);
  status = this->get("status?id=living-room");
  EXPECT_EQ(autotune_of(status)["reason"], "cancelled");
}

TEST_F(Editor, AutotuneReadsItsParametersBeforeTheThermostat) {
  EXPECT_EQ(this->post("autotune?value=true").error(), "Missing id parameter");
  EXPECT_EQ(this->post("autotune?id=Living&value=true").error(), "Invalid id parameter");
  EXPECT_EQ(this->post("autotune?id=nope").error(), "Missing value parameter");
  EXPECT_EQ(this->post("autotune?id=nope&value=1").error(), "Invalid value parameter");
  EXPECT_EQ(this->post("autotune?id=nope&value=true&direction=up").error(), "Invalid direction parameter");
  EXPECT_EQ(this->post("autotune?id=nope&value=true&rule=zn").error(), "Invalid rule parameter");
  Reply missing = this->post("autotune?id=nope&value=true&direction=cool&rule=no_overshoot");
  EXPECT_EQ(missing.code, 404);
  EXPECT_EQ(missing.error(), "Thermostat not found");
  EXPECT_EQ(this->get("autotune?id=nope&value=true").code, 405);
}

TEST_F(Editor, EveryRuleNameIsTaken) {
  ASSERT_EQ(this->create(LIVING_ROOM), "living-room");
  for (const char *rule : {"zn_pi", "zn_pid", "pessen", "some_overshoot", "no_overshoot"}) {
    ASSERT_EQ(this->post(std::string("autotune?id=living-room&value=true&rule=") + rule).code, 200) << rule;
    Reply status = this->get("status?id=living-room");
    EXPECT_EQ(autotune_of(status)["rule"], rule);
    ASSERT_EQ(this->post("autotune?id=living-room&value=false").code, 200);
  }
}

}  // namespace esphome::web_climate_editor::testing
