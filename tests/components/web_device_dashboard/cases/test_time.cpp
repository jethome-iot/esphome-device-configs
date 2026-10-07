#include "common.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/components/timezone_settings/posix_tz_parse.h"
#include "esphome/components/timezone_settings/timezone_settings.h"

namespace esphome::web_device_dashboard::testing {

using timezone_settings::ClockSource;

static const char *const TIME = "/api/device/time";
static const char *const BERLIN = "CET-1CEST,M3.5.0,M10.5.0/3";
static const char *const TOKYO_BODY = R"({"follow_homeassistant":false,"zone":"Asia/Tokyo","posix":"JST-9"})";
// 2026-07-15 12:00 UTC: Berlin is on summer time.
static const time_t JULY = 1784116800;

class FakeClock : public time::RealTimeClock {
 public:
  void update() override {}
  void sync() { this->time_sync_callback_.call(); }
};

// The settings with their instant and their flash in reach; the component's own suite covers
// the rest of what they do.
class TestZone : public timezone_settings::TimezoneSettings {
 public:
  time_t now{JULY};
  bool flush_fails{false};

 protected:
  time_t now_() const override { return this->now; }
  bool store_(const timezone_settings::StoredSettings &stored) override {
    return !this->flush_fails && TimezoneSettings::store_(stored);
  }
};

class Time : public Dashboard {
 protected:
  void SetUp() override {
    Dashboard::SetUp();
    time::set_global_tz(time::ParsedTimezone{});
    this->zone.set_default_zone("Europe/Berlin", BERLIN);
    this->zone.set_preference_hash(fnv1_hash("zone"));
    this->zone.add_clock(&this->homeassistant, ClockSource::HOMEASSISTANT);
    this->zone.add_clock(&this->sntp, ClockSource::SNTP);
    this->zone.setup();
    this->dashboard->set_timezone_settings(&this->zone);
  }

  Reply set(const std::string &body) { return this->post(TIME, body); }

  static bool in_effect(const char *posix) {
    time::ParsedTimezone tz{};
    EXPECT_TRUE(timezone_settings::parse_posix_tz(posix, tz)) << posix;
    return timezone_settings::same_zone(time::get_global_tz(), tz);
  }

  FakeClock homeassistant;
  FakeClock sntp;
  TestZone zone;
};

TEST_F(Dashboard, CapabilitiesLeaveTheTimeOutWithoutItsSettings) {
  Reply reply = this->get("/api/device/capabilities");
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply["time"].isUnbound()) << reply.body;
}

TEST_F(Dashboard, TheTimeRouteIsNotFoundWithoutItsSettings) {
  Reply read = this->get(TIME);
  EXPECT_EQ(read.code, 404);
  EXPECT_EQ(read.error(), "No time settings");
  Reply write = this->post(TIME, R"({"follow_homeassistant":true})");
  EXPECT_EQ(write.code, 404);
  EXPECT_EQ(write.error(), "No time settings");
}

TEST_F(Time, CapabilitiesSayTheTimeIsThere) {
  Reply reply = this->get("/api/device/capabilities");
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply["time"].is<bool>()) << reply.body;
  EXPECT_TRUE(reply["time"].as<bool>());
}

TEST_F(Time, TheRouteIsLookedUpByItsName) {
  const Route *route = this->dashboard->route_for_(TIME);
  ASSERT_NE(route, nullptr);
  EXPECT_EQ(route->id, RouteId::TIME);
  EXPECT_EQ(this->dashboard->route_for_("/api/device/time/zone"), nullptr);
}

// The whole body, so the order, the types and every key that should not be there are covered.
TEST_F(Time, GetAnswersWhatTheDashboardReads) {
  Reply reply = this->get(TIME);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.type, "application/json");
  EXPECT_EQ(reply.body, R"({"time":1784116800,"utc_offset":7200,"dst":true,"clock_source":null,)"
                        R"("follow_homeassistant":true,"zone_source":"default","zone":null,"posix":null,)"
                        R"("default_zone":"Europe/Berlin"})");
}

TEST_F(Time, GetWithoutTheTimeGivesTheStandardOffset) {
  this->zone.now = 0;
  Reply reply = this->get(TIME);
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply["time"].isNull());
  EXPECT_FALSE(reply["time"].isUnbound());
  EXPECT_EQ(reply["utc_offset"].as<int>(), 3600);
  EXPECT_FALSE(reply["dst"].as<bool>());
}

TEST_F(Time, GetNamesTheClockThatSyncedLast) {
  this->sntp.sync();
  EXPECT_EQ(this->get(TIME)["clock_source"].as<std::string>(), "sntp");
  this->homeassistant.sync();
  EXPECT_EQ(this->get(TIME)["clock_source"].as<std::string>(), "homeassistant");
  this->loop();
}

TEST_F(Time, GetIsReadOnTheLoopTask) {
  const int before = this->dashboard->jobs;
  EXPECT_EQ(this->get(TIME).code, 200);
  EXPECT_EQ(this->dashboard->jobs, before + 1);
  this->dashboard->loop_busy = true;
  Reply reply = this->get(TIME);
  EXPECT_EQ(reply.code, 503);
  EXPECT_EQ(reply.error(), "Device busy");
}

TEST_F(Time, AChosenZoneAppliesAtOnce) {
  Reply reply = this->set(TOKYO_BODY);
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"time":1784116800,"utc_offset":32400,"dst":false,"clock_source":null,)"
                        R"("follow_homeassistant":false,"zone_source":"manual","zone":"Asia/Tokyo",)"
                        R"("posix":"JST-9","default_zone":"Europe/Berlin"})");
  EXPECT_TRUE(this->in_effect("JST-9"));
  // And what GET answers next.
  EXPECT_EQ(this->get(TIME).body, reply.body);
}

TEST_F(Time, FollowingHomeAssistantAgainKeepsTheChosenZone) {
  ASSERT_EQ(this->set(TOKYO_BODY).code, 200);
  Reply reply = this->set(R"({"follow_homeassistant":true})");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_TRUE(reply["follow_homeassistant"].as<bool>());
  EXPECT_EQ(reply["zone_source"].as<std::string>(), "default");
  EXPECT_EQ(reply["utc_offset"].as<int>(), 7200);
  EXPECT_EQ(reply["zone"].as<std::string>(), "Asia/Tokyo");
  EXPECT_EQ(reply["posix"].as<std::string>(), "JST-9");
  EXPECT_TRUE(this->in_effect(BERLIN));
}

TEST_F(Time, NotFollowingTakesTheStoredZone) {
  ASSERT_EQ(this->set(TOKYO_BODY).code, 200);
  ASSERT_EQ(this->set(R"({"follow_homeassistant":true})").code, 200);
  Reply reply = this->set(R"({"follow_homeassistant":false})");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply["zone_source"].as<std::string>(), "manual");
  EXPECT_EQ(reply["utc_offset"].as<int>(), 32400);
  EXPECT_TRUE(this->in_effect("JST-9"));
}

TEST_F(Time, NotFollowingWithNoZoneStoredIsRefused) {
  Reply reply = this->set(R"({"follow_homeassistant":false})");
  EXPECT_EQ(reply.code, 422);
  EXPECT_EQ(reply.error(), "No zone was chosen yet: send 'posix' with it");
  EXPECT_TRUE(this->get(TIME)["follow_homeassistant"].as<bool>());
}

// While following, a zone is stored for later rather than applied, whether the body says so or
// leaves the mode out.
TEST_F(Time, AZoneSentWhileFollowingIsKeptNotApplied) {
  for (const char *body : {R"({"zone":"Asia/Tokyo","posix":"JST-9"})",
                           R"({"follow_homeassistant":true,"zone":"Asia/Kolkata","posix":"IST-5:30"})"}) {
    Reply reply = this->set(body);
    ASSERT_EQ(reply.code, 200) << body << " " << reply.body;
    EXPECT_TRUE(reply["follow_homeassistant"].as<bool>()) << body;
    EXPECT_EQ(reply["zone_source"].as<std::string>(), "default") << body;
    EXPECT_EQ(reply["utc_offset"].as<int>(), 7200) << body;
    EXPECT_TRUE(this->in_effect(BERLIN)) << body;
  }
  Reply reply = this->get(TIME);
  EXPECT_EQ(reply["zone"].as<std::string>(), "Asia/Kolkata");
  EXPECT_EQ(reply["posix"].as<std::string>(), "IST-5:30");
}

TEST_F(Time, AZoneSentWhileNotFollowingReplacesTheOneInEffect) {
  ASSERT_EQ(this->set(TOKYO_BODY).code, 200);
  Reply reply = this->set(R"({"zone":"Asia/Kolkata","posix":"IST-5:30"})");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_FALSE(reply["follow_homeassistant"].as<bool>());
  EXPECT_EQ(reply["utc_offset"].as<int>(), 19800);
  EXPECT_TRUE(this->in_effect("IST-5:30"));
}

TEST_F(Time, AZoneMayComeWithoutAName) {
  for (const char *body : {R"({"follow_homeassistant":false,"posix":"<+0330>-3:30"})",
                           R"({"follow_homeassistant":false,"zone":null,"posix":"<+0330>-3:30"})",
                           R"({"follow_homeassistant":false,"zone":"","posix":"<+0330>-3:30"})"}) {
    Reply reply = this->set(body);
    ASSERT_EQ(reply.code, 200) << body << " " << reply.body;
    EXPECT_TRUE(reply["zone"].isNull()) << body;
    EXPECT_FALSE(reply["zone"].isUnbound()) << body;
    EXPECT_EQ(reply["posix"].as<std::string>(), "<+0330>-3:30") << body;
    EXPECT_EQ(reply["utc_offset"].as<int>(), 12600) << body;
  }
}

TEST_F(Time, AnUnknownKeyBesideAKnownOneIsIgnored) {
  Reply reply = this->set(R"({"follow_homeassistant":true,"note":"from a newer page"})");
  EXPECT_EQ(reply.code, 200) << reply.body;
}

TEST_F(Time, WhatIsNotARequestIsABadRequest) {
  struct Case {
    const char *body;
    const char *error;
  };
  for (const Case &c : {
           Case{"", "Invalid JSON"},
           Case{"not json", "Invalid JSON"},
           Case{"[true]", "Invalid JSON"},
           Case{"{}", "Expected 'follow_homeassistant', 'posix' or 'zone'"},
           Case{R"({"timezone":"Asia/Tokyo"})", "Expected 'follow_homeassistant', 'posix' or 'zone'"},
           Case{R"({"follow_homeassistant":"yes"})", "'follow_homeassistant' must be true or false"},
           Case{R"({"follow_homeassistant":null})", "'follow_homeassistant' must be true or false"},
           Case{R"({"follow_homeassistant":1})", "'follow_homeassistant' must be true or false"},
           Case{R"({"zone":3,"posix":"JST-9"})", "'zone' must be a string or null"},
           Case{R"({"zone":["Asia/Tokyo"],"posix":"JST-9"})", "'zone' must be a string or null"},
           Case{R"({"posix":9})", "'posix' must be a string"},
           Case{R"({"posix":null})", "'posix' must be a string"},
           Case{R"({"zone":"Asia/Tokyo","posix":{"std":-32400}})", "'posix' must be a string"},
       }) {
    Reply reply = this->set(c.body);
    EXPECT_EQ(reply.code, 400) << c.body;
    EXPECT_EQ(reply.error(), c.error) << c.body;
  }
  EXPECT_FALSE(this->zone.has_chosen_zone());
  EXPECT_TRUE(this->zone.follow_homeassistant());
}

TEST_F(Time, WhatTheDeviceCannotTakeIsUnprocessable) {
  const std::string long_posix = "JST-9" + std::string(59, ' ');
  const std::string long_zone(64, 'z');
  struct Case {
    std::string body;
    const char *error;
  };
  for (const Case &c : {
           Case{R"({"zone":"Asia/Tokyo"})", "'zone' needs 'posix'"},
           Case{R"({"follow_homeassistant":false,"zone":"Asia/Tokyo"})", "'zone' needs 'posix'"},
           Case{R"({"zone":null})", "'zone' needs 'posix'"},
           Case{R"({"posix":"Asia/Tokyo"})", "'posix' is not a POSIX TZ string"},
           Case{R"({"follow_homeassistant":false,"posix":""})", "'posix' is not a POSIX TZ string"},
           Case{R"({"posix":"JST-9\u0000x"})", "'posix' is not a POSIX TZ string"},
           Case{R"({"posix":")" + long_posix + R"("})", "'posix' is over 63 bytes"},
           Case{R"({"zone":")" + long_zone + R"(","posix":"JST-9"})", "'zone' is over 63 bytes"},
           Case{R"({"zone":"Asia/\u0001Tokyo","posix":"JST-9"})", "'zone' cannot contain control characters"},
       }) {
    Reply reply = this->set(c.body);
    EXPECT_EQ(reply.code, 422) << c.body;
    EXPECT_EQ(reply.error(), c.error) << c.body;
  }
  EXPECT_FALSE(this->zone.has_chosen_zone());
  EXPECT_TRUE(this->in_effect(BERLIN));
}

TEST_F(Time, AStoreThatFailsIsAServerErrorAndChangesNothing) {
  this->zone.flush_fails = true;
  Reply reply = this->set(TOKYO_BODY);
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "The time settings were not stored");
  Reply after = this->get(TIME);
  EXPECT_TRUE(after["follow_homeassistant"].as<bool>());
  EXPECT_TRUE(after["posix"].isNull());
  EXPECT_TRUE(this->in_effect(BERLIN));
}

TEST_F(Time, ALoopTaskThatDoesNotTakeTheChangeChangesNothing) {
  this->dashboard->loop_busy = true;
  Reply reply = this->set(TOKYO_BODY);
  EXPECT_EQ(reply.code, 503);
  EXPECT_EQ(reply.error(), "Device busy");
  EXPECT_FALSE(this->zone.has_chosen_zone());
}

TEST_F(Time, PostTakesJsonAndNothingElse) {
  Reply form = this->call(HTTP_POST, TIME, TOKYO_BODY, 512, "text/plain");
  EXPECT_EQ(form.code, 415);
  EXPECT_EQ(form.error(), "Expected Content-Type: application/json");
  std::string big = TOKYO_BODY;
  big.resize(4097, ' ');
  Reply large = this->set(big);
  EXPECT_EQ(large.code, 413);
  EXPECT_EQ(large.error(), "Request body over 4 KiB");
  EXPECT_FALSE(this->zone.has_chosen_zone());
}

TEST_F(Time, TakesGetAndPostOnly) {
  for (http_method method : {HTTP_PUT, HTTP_DELETE}) {
    LogCapture::instance().clear();
    Reply reply = this->call(method, TIME);
    EXPECT_EQ(reply.code, 405) << method;
    EXPECT_TRUE(LogCapture::instance().has_warning("allowed: GET, POST")) << method;
  }
}

}  // namespace esphome::web_device_dashboard::testing
