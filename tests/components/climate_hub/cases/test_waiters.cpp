// Thermostats that wait for a relay. An enabled one reserves the relays it names, so the editor
// never makes two enabled ones on a relay; two only come from files written by hand or a
// restore, and then the one that waits starts as soon as the relay is free.
#include "common.h"

namespace esphome::climate_hub::testing {

namespace {

// A document as a file holds it: `cool` "" for heat only, `heat` "" for cool only.
std::string file_doc(const char *id, const char *name, const char *heat, const char *cool = "", bool enabled = true,
                     const char *sensor = "room") {
  const char *mode = *heat == '\0' ? "cool" : *cool == '\0' ? "heat" : "heat_cool";
  char buf[512];
  snprintf(buf, sizeof(buf),
           R"({"version":1,"id":"%s","name":"%s","enabled":%s,"kind":"bang_bang","sensor_id":"%s",)"
           R"("heat":{"relay_id":"%s"},"cool":{"relay_id":"%s"},"mode":"%s","setpoint":21})",
           id, name, enabled ? "true" : "false", sensor, heat, cool, mode);
  return buf;
}

using Ids = std::vector<std::string>;

}  // namespace

// --- An enabled thermostat that waits reserves its relays (D) ---

TEST_F(HubTest, AnEnabledThermostatThatWaitsReservesItsRelay) {
  ClimateConfig attic = draft("Attic");
  attic.sensor_id = "gone";
  ASSERT_EQ("not started: sensor 'gone' not found", this->create(attic).warning);
  ASSERT_EQ("", hub().claimed_by("relay_1")) << "it holds nothing";
  const std::string refusal = "\"Relay 1\" is reserved by \"Attic\", which is enabled and waits to start";

  Result created = hub().create(draft("Boiler"));
  EXPECT_EQ(409, created.code);
  EXPECT_EQ(refusal, created.error);
  EXPECT_EQ("attic", created.holder) << "for a take-over";
  EXPECT_EQ(nullptr, hub().store().get("boiler"));
  EXPECT_EQ("", hub().claimed_by("relay_1"));

  // Stored disabled it takes nothing, and stays so.
  ClimateConfig boiler = draft("Boiler");
  boiler.enabled = false;
  this->create(boiler);
  const std::string stored = read_file(this->file_of("boiler"));
  Result updated = hub().update("boiler", draft("Boiler"));
  EXPECT_EQ(409, updated.code);
  EXPECT_EQ(refusal, updated.error);
  Result enabled = hub().set_enabled("boiler", true);
  EXPECT_EQ(409, enabled.code);
  EXPECT_EQ(refusal, enabled.error);
  EXPECT_EQ("attic", enabled.holder);
  EXPECT_FALSE(hub().store().get("boiler")->enabled);
  EXPECT_EQ(stored, read_file(this->file_of("boiler")));

  // Its other relays are free for anyone.
  Result porch = hub().create(draft("Porch", "relay_2"));
  EXPECT_TRUE(porch.ok) << porch.error;
  EXPECT_TRUE(hub().is_running("porch"));
}

// The cooling relay counts as much as the heating one, on either side.
TEST_F(HubTest, AReservedCoolingRelayIsRefusedToo) {
  ClimateConfig attic = draft("Attic", "relay_2");
  attic.cool.relay_id = "relay_3";
  attic.mode = HubMode::HEAT_COOL;
  attic.sensor_id = "gone";
  this->create(attic);

  ClimateConfig garage = draft("Garage", "");
  garage.cool.relay_id = "relay_3";
  garage.mode = HubMode::COOL;
  Result result = hub().create(garage);
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("\"Relay 3\" is reserved by \"Attic\", which is enabled and waits to start", result.error);

  ClimateConfig shed = draft("Shed", "relay_1");
  shed.cool.relay_id = "relay_2";
  shed.mode = HubMode::HEAT_COOL;
  result = hub().create(shed);
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("\"Relay 2\" is reserved by \"Attic\", which is enabled and waits to start", result.error);
}

TEST_F(HubTest, ADisabledThermostatReservesNothing) {
  ClimateConfig attic = draft("Attic");
  attic.sensor_id = "gone";
  attic.enabled = false;
  this->create(attic);

  Result result = hub().create(draft("Boiler"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(hub().is_running("boiler"));
}

// A relay a running thermostat holds is its own: whoever waits for it, its Save goes through,
// and so does its own retry for one that only waits.
TEST_F(HubTest, ARunningThermostatsSaveIsNotRefusedForOneThatWaitsOnItsRelay) {
  write_file(this->file_of("summer"), file_doc("summer", "Summer", "relay_1"));
  write_file(this->file_of("winter"), file_doc("winter", "Winter", "relay_1"));
  this->reboot();
  ASSERT_TRUE(hub().is_running("summer"));
  ASSERT_FALSE(hub().is_running("winter"));

  ClimateConfig warmer = draft("Summer");
  warmer.setpoint = 24.f;
  Result result = hub().update("summer", warmer);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(hub().is_running("summer"));
  EXPECT_EQ("summer", hub().claimed_by("relay_1"));
  EXPECT_TRUE(result.started.empty());
  EXPECT_FALSE(hub().is_running("winter"));

  // The one that waits is still refused by the holder, as before.
  Result refused = hub().update("winter", draft("Winter"));
  EXPECT_EQ(409, refused.code);
  EXPECT_EQ("\"Relay 1\" is already driven by \"Summer\"", refused.error);
}

// A Save that moves a running thermostat onto a relay another one waits for is refused, and
// leaves it where it was.
TEST_F(HubTest, ASaveOntoARelayAnotherWaitsForIsRefused) {
  this->create(draft("Boiler", "relay_2"));
  ClimateConfig attic = draft("Attic");
  attic.sensor_id = "gone";
  this->create(attic);
  const std::string stored = read_file(this->file_of("boiler"));

  Result result = hub().update("boiler", draft("Boiler", "relay_1"));
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("attic", result.holder);
  EXPECT_EQ("boiler", hub().claimed_by("relay_2"));
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  EXPECT_EQ(stored, read_file(this->file_of("boiler")));
}

// --- Taking a relay over from one that waits ---

TEST_F(HubTest, ATakeOverDisablesAThermostatThatWaits) {
  ClimateConfig attic = draft("Attic");
  attic.sensor_id = "gone";
  this->create(attic);
  ClimateConfig boiler = draft("Boiler");
  boiler.enabled = false;
  this->create(boiler);

  Result result = hub().set_enabled("boiler", true, true);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.persisted);
  EXPECT_EQ(Ids{"attic"}, result.stopped);
  EXPECT_TRUE(result.started.empty());
  EXPECT_TRUE(hub().is_running("boiler"));
  EXPECT_EQ("boiler", hub().claimed_by("relay_1"));
  EXPECT_FALSE(hub().store().get("attic")->enabled);
  EXPECT_EQ("", hub().waiting_reason("attic"));
  EXPECT_EQ(0u, hub().reasons_kept());

  this->reboot();
  EXPECT_FALSE(hub().store().get("attic")->enabled) << "stored disabled";
  EXPECT_TRUE(hub().is_running("boiler"));
}

// The holder and everyone else enabled on the relay, in one step: running ones first.
TEST_F(HubTest, ATakeOverDisablesTheHolderAndEveryThermostatWaitingOnTheRelay) {
  write_file(this->file_of("summer"), file_doc("summer", "Summer", "relay_1"));
  write_file(this->file_of("winter"), file_doc("winter", "Winter", "relay_1"));
  write_file(this->file_of("autumn"), file_doc("autumn", "Autumn", "relay_1", "", false));
  this->reboot();
  ASSERT_TRUE(hub().is_running("summer"));

  Result result = hub().set_enabled("autumn", true, true);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ((Ids{"summer", "winter"}), result.stopped);
  EXPECT_TRUE(result.started.empty()) << "the relay went to the one taking it over";
  EXPECT_TRUE(hub().is_running("autumn"));
  EXPECT_EQ("autumn", hub().claimed_by("relay_1"));
  for (const char *id : {"summer", "winter"}) {
    EXPECT_FALSE(hub().store().get(id)->enabled) << id;
    EXPECT_FALSE(hub().is_running(id)) << id;
  }
  EXPECT_EQ(0u, hub().reasons_kept());

  this->reboot();
  EXPECT_TRUE(hub().is_running("autumn"));
  EXPECT_FALSE(hub().store().get("summer")->enabled);
  EXPECT_FALSE(hub().store().get("winter")->enabled);
}

// Disabling one that waits is no reason to refuse anything: it is refused, before anything
// moves, only for a thermostat that would not run in its place.
TEST_F(HubTest, ATakeOverFromOneThatWaitsNeedsTheSensorToo) {
  ClimateConfig attic = draft("Attic");
  attic.sensor_id = "gone";
  this->create(attic);
  ClimateConfig porch = draft("Porch");
  porch.sensor_id = "missing";
  porch.enabled = false;
  this->create(porch);

  Result result = hub().set_enabled("porch", true, true);
  EXPECT_EQ(400, result.code);
  EXPECT_EQ("No sensor \"missing\" on this device", result.error);
  EXPECT_TRUE(hub().store().get("attic")->enabled);
  EXPECT_EQ("not started: sensor 'gone' not found", hub().waiting_reason("attic"));
  EXPECT_FALSE(hub().store().get("porch")->enabled);
}

// --- The one that waits starts when the relay comes free (A) ---

TEST_F(HubTest, RemovingTheHolderStartsTheThermostatThatWaits) {
  write_file(this->file_of("summer"), file_doc("summer", "Summer", "relay_1"));
  write_file(this->file_of("winter"), file_doc("winter", "Winter", "relay_1"));
  this->reboot();
  LogCapture::instance().clear();

  Result result = hub().remove("summer");
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(Ids{"winter"}, result.started);
  EXPECT_TRUE(hub().is_running("winter"));
  EXPECT_EQ("winter", hub().claimed_by("relay_1"));
  EXPECT_EQ("", hub().waiting_reason("winter"));
  EXPECT_TRUE(LogCapture::instance().has("'winter' started: a relay it waited for is free"));
}

// Two that wait for one relay: the first by id takes it, and the next one's reason names it.
TEST_F(HubTest, TheFirstWaiterByIdTakesTheRelayAndTheNextSaysWho) {
  write_file(this->file_of("a"), file_doc("a", "A", "relay_1"));
  write_file(this->file_of("c"), file_doc("c", "C", "relay_1"));
  write_file(this->file_of("b"), file_doc("b", "B", "relay_1"));
  this->reboot();
  ASSERT_TRUE(hub().is_running("a"));
  ASSERT_EQ("not started: relay 'relay_1' is held by 'a'", hub().waiting_reason("c"));

  Result result = hub().set_enabled("a", false);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(Ids{"b"}, result.started);
  EXPECT_TRUE(hub().is_running("b"));
  EXPECT_FALSE(hub().is_running("c"));
  EXPECT_EQ("not started: relay 'relay_1' is held by 'b'", hub().waiting_reason("c"));
}

// One that still cannot start keeps waiting, for the reason it has now, and touches no relay.
TEST_F(HubTest, AWaiterThatStillCannotStartWaitsWithAFreshReason) {
  write_file(this->file_of("summer"), file_doc("summer", "Summer", "relay_1"));
  write_file(this->file_of("winter"), file_doc("winter", "Winter", "relay_1", "relay_9"));
  this->reboot();
  ASSERT_EQ("not started: relay 'relay_1' is held by 'summer'", hub().waiting_reason("winter"));
  LogCapture::instance().clear();

  Result result = hub().set_enabled("summer", false);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.started.empty());
  EXPECT_FALSE(hub().is_running("winter"));
  EXPECT_TRUE(hub().store().get("winter")->enabled);
  EXPECT_EQ("not started: relay 'relay_9' not found", hub().waiting_reason("winter"));
  EXPECT_TRUE(LogCapture::instance().has("'winter' not started: relay 'relay_9' not found"));
  EXPECT_EQ("", hub().claimed_by("relay_1"));
  EXPECT_FALSE(entities().relay1.state);
}

// The relays a holder drove alone come free with the take-over: the one taking over starts
// first, then whoever waits for those.
TEST_F(HubTest, AfterATakeOverTheWaiterOnTheHoldersOtherRelayStartsSecond) {
  write_file(this->file_of("house"), file_doc("house", "House", "relay_1", "relay_2"));
  write_file(this->file_of("porch"), file_doc("porch", "Porch", "relay_2"));
  write_file(this->file_of("study"), file_doc("study", "Study", "relay_1", "", false));
  this->reboot();
  ASSERT_EQ("not started: relay 'relay_2' is held by 'house'", hub().waiting_reason("porch"));
  LogCapture::instance().clear();

  Result result = hub().set_enabled("study", true, true);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(Ids{"house"}, result.stopped);
  EXPECT_EQ(Ids{"porch"}, result.started);
  EXPECT_EQ("study", hub().claimed_by("relay_1"));
  EXPECT_EQ("porch", hub().claimed_by("relay_2"));
  const auto &lines = LogCapture::instance().lines;
  auto line_of = [&lines](const std::string &needle) {
    return std::distance(lines.begin(), std::find_if(lines.begin(), lines.end(), [&needle](const std::string &line) {
                           return line.find(needle) != std::string::npos;
                         }));
  };
  ASSERT_LT(line_of("'porch' running as climate"), static_cast<long>(lines.size()));
  EXPECT_LT(line_of("'study' running as climate"), line_of("'porch' running as climate"));
}

// A Save that swaps a relay frees the old one. Swapping costs Home Assistant no reconnect, but
// the waiter's new entity does.
TEST_F(HubTest, ASaveOntoAnotherRelayStartsTheWaiterOnTheOldOne) {
  write_file(this->file_of("summer"), file_doc("summer", "Summer", "relay_1"));
  write_file(this->file_of("winter"), file_doc("winter", "Winter", "relay_1"));
  this->reboot();
  pass_resync_delay();
  hub().resyncs = 0;

  ClimateConfig moved = draft("Summer", "relay_2");
  moved.kind = ControlKind::BANG_BANG;
  Result result = hub().update("summer", moved);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(Ids{"winter"}, result.started);
  EXPECT_EQ("summer", hub().claimed_by("relay_2"));
  EXPECT_EQ("winter", hub().claimed_by("relay_1"));
  pass_resync_delay();
  EXPECT_EQ(1, hub().resyncs);
}

// So does a Save that drops a relay, or switches the thermostat off.
TEST_F(HubTest, ASaveThatDropsARelayOrDisablesStartsItsWaiter) {
  write_file(this->file_of("house"), file_doc("house", "House", "relay_1", "relay_2"));
  write_file(this->file_of("porch"), file_doc("porch", "Porch", "relay_2"));
  write_file(this->file_of("study"), file_doc("study", "Study", "relay_1"));
  this->reboot();

  Result result = hub().update("house", draft("House", "relay_1"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(Ids{"porch"}, result.started);
  EXPECT_EQ("porch", hub().claimed_by("relay_2"));

  ClimateConfig off = draft("House", "relay_1");
  off.enabled = false;
  result = hub().update("house", off);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(Ids{"study"}, result.started);
  EXPECT_EQ("study", hub().claimed_by("relay_1"));
}

// A Save that leaves the thermostat waiting lets its relay go to the next in line, and is not
// tried again itself: its warning already says why.
TEST_F(HubTest, ASaveThatLeavesItWaitingHandsTheRelayOn) {
  write_file(this->file_of("summer"), file_doc("summer", "Summer", "relay_1"));
  write_file(this->file_of("winter"), file_doc("winter", "Winter", "relay_1"));
  this->reboot();
  LogCapture::instance().clear();

  ClimateConfig moved = draft("Summer");
  moved.sensor_id = "gone";
  Result result = hub().update("summer", moved);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ("not started: sensor 'gone' not found", result.warning);
  EXPECT_EQ(Ids{"winter"}, result.started);
  EXPECT_EQ("winter", hub().claimed_by("relay_1"));
  EXPECT_EQ(result.warning, hub().waiting_reason("summer"));
  EXPECT_FALSE(LogCapture::instance().has("'summer' not started")) << "not retried";
}

// Nothing freed, nothing started: a stop of one that waits, a create, a plain enable.
TEST_F(HubTest, NoRelayFreedStartsNobody) {
  write_file(this->file_of("summer"), file_doc("summer", "Summer", "relay_1"));
  write_file(this->file_of("winter"), file_doc("winter", "Winter", "relay_1"));
  this->reboot();

  Result result = hub().set_enabled("winter", false);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.started.empty());
  result = hub().set_enabled("winter", true);
  EXPECT_EQ(409, result.code);
  EXPECT_EQ("\"Relay 1\" is already driven by \"Summer\"", result.error);
  result = hub().create(draft("Porch", "relay_2"));
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.started.empty());
  EXPECT_TRUE(result.stopped.empty());
  EXPECT_EQ("summer", hub().claimed_by("relay_1"));
}

}  // namespace esphome::climate_hub::testing
