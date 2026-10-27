#include "common.h"
#include "esphome/components/ntp_servers/ntp_servers.h"
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
// An hour later, which the cases set the clock to.
static const time_t JULY_LATER = JULY + 3600;

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
  // What the system clock was asked to read, and whether it takes it; never the host's clock.
  std::vector<uint32_t> clock_sets;
  bool clock_takes{true};

 protected:
  time_t now_() const override { return this->now; }
  bool set_system_clock_(uint32_t epoch) override {
    this->clock_sets.push_back(epoch);
    if (this->clock_takes)
      this->now = epoch;
    return this->clock_takes;
  }
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

  // The time key alone, as the dashboard sends it.
  Reply set_time(int64_t epoch) { return this->set(str_sprintf(R"({"time":%lld})", static_cast<long long>(epoch))); }

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
           Case{"{}", "Expected 'follow_homeassistant', 'posix', 'zone' or 'time'"},
           Case{R"({"timezone":"Asia/Tokyo"})", "Expected 'follow_homeassistant', 'posix', 'zone' or 'time'"},
           Case{R"({"epoch":1784120400})", "Expected 'follow_homeassistant', 'posix', 'zone' or 'time'"},
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
           // INT32_MIN, which ESPHome's dump_config() would negate on the next boot.
           Case{R"({"follow_homeassistant":false,"posix":"EST-596523:14:08"})", "'posix' is not a POSIX TZ string"},
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

TEST_F(Time, TimeAloneSetsTheClock) {
  Reply reply = this->set_time(JULY_LATER);
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"time":1784120400,"utc_offset":7200,"dst":true,"clock_source":"dashboard",)"
                        R"("follow_homeassistant":true,"zone_source":"default","zone":null,"posix":null,)"
                        R"("default_zone":"Europe/Berlin"})");
  EXPECT_EQ(this->zone.clock_sets, std::vector<uint32_t>{JULY_LATER});
  EXPECT_EQ(this->get(TIME).body, reply.body);
  // Nothing about the zone was sent, so nothing about it was stored or changed.
  EXPECT_FALSE(this->zone.has_chosen_zone());
  EXPECT_TRUE(this->in_effect(BERLIN));
}

TEST_F(Time, TimeWithAZoneSetsBoth) {
  Reply reply = this->set(R"({"follow_homeassistant":false,"zone":"Asia/Tokyo","posix":"JST-9","time":1784120400})");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply.body, R"({"time":1784120400,"utc_offset":32400,"dst":false,"clock_source":"dashboard",)"
                        R"("follow_homeassistant":false,"zone_source":"manual","zone":"Asia/Tokyo",)"
                        R"("posix":"JST-9","default_zone":"Europe/Berlin"})");
  EXPECT_EQ(this->zone.clock_sets, std::vector<uint32_t>{JULY_LATER});
  EXPECT_TRUE(this->in_effect("JST-9"));
}

TEST_F(Time, TimeWithTheModeAloneSetsBoth) {
  ASSERT_EQ(this->set(TOKYO_BODY).code, 200);
  Reply reply = this->set(R"({"follow_homeassistant":true,"time":1784120400})");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_TRUE(reply["follow_homeassistant"].as<bool>());
  EXPECT_EQ(reply["time"].as<int64_t>(), JULY_LATER);
  EXPECT_EQ(reply["clock_source"].as<std::string>(), "dashboard");
  EXPECT_TRUE(this->in_effect(BERLIN));
}

TEST_F(Time, TheTimeIsFrom2019To2099) {
  for (const int64_t epoch : {int64_t{1546300800}, int64_t{4102444799}}) {
    Reply reply = this->set_time(epoch);
    ASSERT_EQ(reply.code, 200) << epoch << " " << reply.body;
    EXPECT_EQ(reply["time"].as<int64_t>(), epoch);
  }
  EXPECT_EQ(this->zone.clock_sets, (std::vector<uint32_t>{1546300800u, 4102444799u}));
  this->zone.clock_sets.clear();
  for (const char *epoch : {"1546300799", "4102444800", "0", "-1", "-9223372036854775808", "9223372036854775807",
                            "9223372036854775808", "18446744073709551615"}) {
    Reply reply = this->set(std::string(R"({"time":)") + epoch + "}");
    EXPECT_EQ(reply.code, 422) << epoch;
    EXPECT_EQ(reply.error(), "'time' must be from 1546300800 (2019-01-01) to 4102444799 (2099-12-31)") << epoch;
  }
  EXPECT_TRUE(this->zone.clock_sets.empty());
}

TEST_F(Time, TheTimeIsAnInteger) {
  for (const char *epoch : {"1784120400.5", "1784120400.0", "1.7841204e9", R"("1784120400")", "null", "true", "false",
                            "[1784120400]", R"({"s":1784120400})", "1e30"}) {
    Reply reply = this->set(std::string(R"({"time":)") + epoch + "}");
    EXPECT_EQ(reply.code, 400) << epoch;
    EXPECT_EQ(reply.error(), "'time' must be an integer") << epoch;
  }
  EXPECT_TRUE(this->zone.clock_sets.empty());
}

// Everything in the body is checked before anything is applied.
TEST_F(Time, ABadTimeLeavesTheZoneAlone) {
  for (const char *body : {R"({"follow_homeassistant":false,"zone":"Asia/Tokyo","posix":"JST-9","time":"now"})",
                           R"({"follow_homeassistant":false,"zone":"Asia/Tokyo","posix":"JST-9","time":0})"}) {
    EXPECT_NE(this->set(body).code, 200) << body;
  }
  EXPECT_FALSE(this->zone.has_chosen_zone());
  EXPECT_TRUE(this->zone.follow_homeassistant());
  EXPECT_TRUE(this->in_effect(BERLIN));
  EXPECT_TRUE(this->zone.clock_sets.empty());
}

TEST_F(Time, ABadZoneLeavesTheClockAlone) {
  struct Case {
    const char *body;
    int code;
    const char *error;
  };
  for (const Case &c : {
           Case{R"({"posix":9,"time":1784120400})", 400, "'posix' must be a string"},
           Case{R"({"follow_homeassistant":"no","time":1784120400})", 400,
                "'follow_homeassistant' must be true or false"},
           Case{R"({"zone":"Asia/Tokyo","time":1784120400})", 422, "'zone' needs 'posix'"},
           Case{R"({"posix":"Asia/Tokyo","time":1784120400})", 422, "'posix' is not a POSIX TZ string"},
           Case{R"({"follow_homeassistant":false,"time":1784120400})", 422,
                "No zone was chosen yet: send 'posix' with it"},
       }) {
    Reply reply = this->set(c.body);
    EXPECT_EQ(reply.code, c.code) << c.body;
    EXPECT_EQ(reply.error(), c.error) << c.body;
  }
  EXPECT_TRUE(this->zone.clock_sets.empty());
  EXPECT_TRUE(this->get(TIME)["clock_source"].isNull());
}

TEST_F(Time, AStoreThatFailsLeavesTheClockAlone) {
  this->zone.flush_fails = true;
  Reply reply = this->set(R"({"follow_homeassistant":false,"zone":"Asia/Tokyo","posix":"JST-9","time":1784120400})");
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "The time settings were not stored");
  EXPECT_TRUE(this->zone.clock_sets.empty());
}

TEST_F(Time, AClockThatDoesNotTakeTheTimeIsAServerError) {
  this->sntp.sync();
  this->zone.clock_takes = false;
  Reply reply = this->set_time(JULY_LATER);
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "The clock could not be set");
  EXPECT_EQ(this->zone.clock_sets, std::vector<uint32_t>{JULY_LATER});
  Reply after = this->get(TIME);
  EXPECT_EQ(after["time"].as<int64_t>(), JULY);
  EXPECT_EQ(after["clock_source"].as<std::string>(), "sntp");
}

// The zone goes first, and is not undone: the error says only that the clock did not take.
TEST_F(Time, AZoneSentWithATimeTheClockRefusesStillApplies) {
  this->zone.clock_takes = false;
  Reply reply = this->set(R"({"follow_homeassistant":false,"zone":"Asia/Tokyo","posix":"JST-9","time":1784120400})");
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "The clock could not be set");
  EXPECT_TRUE(this->in_effect("JST-9"));
}

TEST_F(Time, ALoopTaskThatDoesNotTakeTheTimeSetsNoClock) {
  this->dashboard->loop_busy = true;
  Reply reply = this->set_time(JULY_LATER);
  EXPECT_EQ(reply.code, 503);
  EXPECT_EQ(reply.error(), "Device busy");
  EXPECT_TRUE(this->zone.clock_sets.empty());
}

TEST_F(Time, TheTimeIsSetOnTheLoopTask) {
  const int before = this->dashboard->jobs;
  ASSERT_EQ(this->set_time(JULY_LATER).code, 200);
  EXPECT_EQ(this->dashboard->jobs, before + 1);
}

TEST_F(Time, GetNamesTheDashboardUntilANetworkClockSyncs) {
  ASSERT_EQ(this->set_time(JULY_LATER).code, 200);
  EXPECT_EQ(this->get(TIME)["clock_source"].as<std::string>(), "dashboard");
  this->sntp.sync();
  EXPECT_EQ(this->get(TIME)["clock_source"].as<std::string>(), "sntp");
  ASSERT_EQ(this->set_time(JULY_LATER).code, 200);
  this->homeassistant.sync();
  EXPECT_EQ(this->get(TIME)["clock_source"].as<std::string>(), "homeassistant");
  this->loop();
}

TEST_F(Time, TakesGetAndPostOnly) {
  for (http_method method : {HTTP_PUT, HTTP_DELETE}) {
    LogCapture::instance().clear();
    Reply reply = this->call(method, TIME);
    EXPECT_EQ(reply.code, 405) << method;
    EXPECT_TRUE(LogCapture::instance().has_warning("allowed: GET, POST")) << method;
  }
}

// The NTP servers, which share the route; the component's own suite covers the rest of what
// they do.
class TestNtp : public ntp_servers::NTPServers {
 public:
  bool flush_fails{false};
  std::vector<std::string> handed;

 protected:
  void use_server_(const char *server) override { this->handed.emplace_back(server == nullptr ? "" : server); }
  bool connected_() const override { return true; }
  time_t now_() const override { return JULY; }
  bool store_(const ntp_servers::StoredServers &stored) override {
    return !this->flush_fails && NTPServers::store_(stored);
  }
};

class Ntp : public Time {
 protected:
  void SetUp() override {
    Time::SetUp();
    this->ntp.set_clock(&this->sntp);
    for (const char *server : {"0.pool.ntp.org", "1.pool.ntp.org", "2.pool.ntp.org"})
      this->ntp.add_fallback_server(server);
    this->ntp.set_preference_hash(fnv1_hash("ntp"));
    this->ntp.setup();
    this->dashboard->set_ntp_servers(&this->ntp);
  }

  TestNtp ntp;
};

static const char *const POOL = R"("fallback_servers":["0.pool.ntp.org","1.pool.ntp.org","2.pool.ntp.org"])";

TEST_F(Ntp, GetAnswersTheServersAfterTheZone) {
  Reply reply = this->get(TIME);
  ASSERT_EQ(reply.code, 200);
  const std::string tail = std::string(R"("default_zone":"Europe/Berlin","ntp":{"servers":[],"fallback":true,)") +
                           POOL + R"(,"current":"0.pool.ntp.org","last_sync":null}})";
  ASSERT_GE(reply.body.size(), tail.size());
  EXPECT_EQ(reply.body.substr(reply.body.size() - tail.size()), tail) << reply.body;
}

TEST_F(Ntp, GetNamesTheServerThatAnswered) {
  this->sntp.sync();
  Reply reply = this->get(TIME);
  EXPECT_EQ(reply["ntp"]["last_sync"]["time"].as<int64_t>(), JULY) << reply.body;
  EXPECT_EQ(reply["ntp"]["last_sync"]["server"].as<std::string>(), "0.pool.ntp.org");
}

TEST_F(Ntp, PostSetsTheServersAtOnce) {
  Reply reply = this->set(R"({"ntp":{"servers":["ntp.local","10.0.0.1"],"fallback":false}})");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_EQ(reply["ntp"]["servers"].size(), 2u);
  EXPECT_EQ(reply["ntp"]["servers"][1].as<std::string>(), "10.0.0.1");
  EXPECT_FALSE(reply["ntp"]["fallback"].as<bool>());
  EXPECT_EQ(reply["ntp"]["current"].as<std::string>(), "ntp.local");
  EXPECT_EQ(this->ntp.handed.back(), "ntp.local");
  // The zone's keys answer as GET has them.
  EXPECT_TRUE(reply["follow_homeassistant"].as<bool>());
}

TEST_F(Ntp, WhatIsLeftOutStays) {
  ASSERT_EQ(this->set(R"({"ntp":{"servers":["ntp.local"]}})").code, 200);
  EXPECT_TRUE(this->ntp.fallback());
  ASSERT_EQ(this->set(R"({"ntp":{"fallback":false}})").code, 200);
  EXPECT_EQ(this->ntp.servers(), std::vector<std::string>{"ntp.local"});
  EXPECT_FALSE(this->ntp.fallback());
  Reply reply = this->set(R"({"ntp":{"servers":[]}})");
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply["ntp"]["current"].isNull());
  EXPECT_FALSE(reply["ntp"]["current"].isUnbound());
}

TEST_F(Ntp, TheServersComeWithAZone) {
  Reply reply = this->set(R"({"follow_homeassistant":false,"zone":"Asia/Tokyo","posix":"JST-9",)"
                          R"("ntp":{"servers":["ntp.local"]}})");
  ASSERT_EQ(reply.code, 200) << reply.body;
  EXPECT_TRUE(this->in_effect("JST-9"));
  EXPECT_EQ(this->ntp.servers(), std::vector<std::string>{"ntp.local"});
}

TEST_F(Ntp, WhatIsNotARequestIsABadRequest) {
  struct Case {
    const char *body;
    const char *error;
  };
  for (const Case &c : {
           Case{"{}", "Expected 'follow_homeassistant', 'posix', 'zone', 'time' or 'ntp'"},
           Case{R"({"ntp":null})", "'ntp' must be an object"},
           Case{R"({"ntp":["ntp.local"]})", "'ntp' must be an object"},
           Case{R"({"ntp":{}})", "'ntp' needs 'servers' or 'fallback'"},
           Case{R"({"ntp":{"server":"ntp.local"}})", "'ntp' needs 'servers' or 'fallback'"},
           Case{R"({"ntp":{"servers":"ntp.local"}})", "'ntp.servers' must be a list of strings"},
           Case{R"({"ntp":{"servers":null}})", "'ntp.servers' must be a list of strings"},
           Case{R"({"ntp":{"servers":["ntp.local",1]}})", "'ntp.servers' must be a list of strings"},
           Case{R"({"ntp":{"servers":[null]}})", "'ntp.servers' must be a list of strings"},
           Case{R"({"ntp":{"fallback":"no"}})", "'ntp.fallback' must be true or false"},
           Case{R"({"ntp":{"servers":[],"fallback":null}})", "'ntp.fallback' must be true or false"},
           // A server list before the fallback, and both before what the device refuses.
           Case{R"({"ntp":{"servers":[1],"fallback":1}})", "'ntp.servers' must be a list of strings"},
           Case{R"({"ntp":{"servers":["a","b","c","d"],"fallback":1}})", "'ntp.fallback' must be true or false"},
       }) {
    Reply reply = this->set(c.body);
    EXPECT_EQ(reply.code, 400) << c.body;
    EXPECT_EQ(reply.error(), c.error) << c.body;
  }
  EXPECT_TRUE(this->ntp.servers().empty());
  EXPECT_TRUE(this->ntp.fallback());
}

TEST_F(Ntp, ServersTheDeviceCannotAskAreUnprocessable) {
  const std::string long_name(64, 'a');
  struct Case {
    std::string body;
    const char *error;
  };
  for (const Case &c : {
           Case{R"({"ntp":{"servers":["a","b","c","d"]}})", "'ntp.servers' lists more than 3 servers"},
           Case{R"({"ntp":{"servers":["ntp.local",")" + long_name + R"("]}})", "'ntp.servers' item 2 is over 63 bytes"},
           Case{R"({"ntp":{"servers":["ntp_local"]}})", "'ntp.servers' item 1 is not a host name or an IPv4 address"},
           Case{R"({"ntp":{"servers":["ntp\u0000.local"]}})",
                "'ntp.servers' item 1 is not a host name or an IPv4 address"},
           Case{R"({"ntp":{"servers":[""]}})", "'ntp.servers' item 1 is not a host name or an IPv4 address"},
           Case{R"({"ntp":{"servers":["10.0.0.256"]}})", "'ntp.servers' item 1 is not a host name or an IPv4 address"},
           Case{R"({"ntp":{"servers":["a.local","b.local","A.LOCAL"]}})", "'ntp.servers' item 3 repeats item 1"},
       }) {
    Reply reply = this->set(c.body);
    EXPECT_EQ(reply.code, 422) << c.body;
    EXPECT_EQ(reply.error(), c.error) << c.body;
  }
  EXPECT_TRUE(this->ntp.servers().empty());
}

// Everything is checked before anything changes.
TEST_F(Ntp, RefusedServersLeaveTheZoneAlone) {
  Reply reply = this->set(R"({"follow_homeassistant":false,"zone":"Asia/Tokyo","posix":"JST-9",)"
                          R"("ntp":{"servers":["ntp_local"]}})");
  EXPECT_EQ(reply.code, 422);
  EXPECT_FALSE(this->zone.has_chosen_zone());
  EXPECT_TRUE(this->in_effect(BERLIN));
}

TEST_F(Ntp, AStoreThatFailsIsAServerError) {
  this->ntp.flush_fails = true;
  Reply reply = this->set(R"({"ntp":{"servers":["ntp.local"]}})");
  EXPECT_EQ(reply.code, 500);
  EXPECT_EQ(reply.error(), "The NTP servers were not stored");
  EXPECT_TRUE(this->ntp.servers().empty());
}

// Stored first, the zone stays in force, as with a clock the device refuses.
TEST_F(Ntp, AZoneSentWithServersThatAreNotStoredStillApplies) {
  this->ntp.flush_fails = true;
  Reply reply = this->set(R"({"follow_homeassistant":false,"zone":"Asia/Tokyo","posix":"JST-9",)"
                          R"("ntp":{"servers":["ntp.local"]}})");
  EXPECT_EQ(reply.code, 500);
  EXPECT_TRUE(this->in_effect("JST-9"));
}

TEST_F(Ntp, ALoopTaskThatDoesNotTakeTheServersChangesNothing) {
  this->dashboard->loop_busy = true;
  Reply reply = this->set(R"({"ntp":{"servers":["ntp.local"]}})");
  EXPECT_EQ(reply.code, 503);
  EXPECT_EQ(reply.error(), "Device busy");
  EXPECT_TRUE(this->ntp.servers().empty());
}

// Without the servers wired in, `ntp` is a key this route does not know.
TEST_F(Time, NtpIsUnknownWithoutTheServers) {
  Reply reply = this->set(R"({"ntp":{"servers":["ntp.local"]}})");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Expected 'follow_homeassistant', 'posix', 'zone' or 'time'");
  EXPECT_TRUE(this->get(TIME)["ntp"].isUnbound());
}

}  // namespace esphome::web_device_dashboard::testing
