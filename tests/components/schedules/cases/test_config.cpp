// The document: its file format byte for byte, every rule it is refused for and with which
// sentence, names, ids and the version.
#include "common.h"

namespace esphome::schedules::testing {

namespace {

// The spec's example.
const char *const GOLDEN = R"({"version":1,"revision":4,"id":"home-weekdays","name":"Home weekdays","enabled":true,)"
                           R"("modes":[{"id":"day","name":"Day","color":"amber","actions":[)"
                           R"({"type":"thermostat","id":"living-room","value":{"type":"preset","preset":"comfort"}},)"
                           R"({"type":"relay","id":"boiler_pump","value":{"type":"on"}},)"
                           R"({"type":"wait","seconds":120},)"
                           R"({"type":"relay","id":"relay_5","value":{"type":"on"}}]},)"
                           R"({"id":"night","name":"Night","color":"indigo","actions":[)"
                           R"({"type":"thermostat","id":"living-room","value":{"type":"preset","preset":"eco"}},)"
                           R"({"type":"relay","id":"relay_5","value":{"type":"off"}},)"
                           R"({"type":"relay","id":"boiler_pump","value":{"type":"off"}}]}],)"
                           R"("week":{"mon":[{"at":"06:30","mode":"day"},{"at":"23:00","mode":"night"}],)"
                           R"("tue":[],"wed":[],"thu":[],"fri":[],"sat":[],"sun":[]}})";

const char *const ON_MODE =
    R"({"id":"on","name":"On","actions":[{"type":"relay","id":"relay_1","value":{"type":"on"}}]})";
const char *const ON_AT_SIX = R"({"mon":[{"at":"06:00","mode":"on"}]})";

// A document of `modes` and `week`, the rest valid.
std::string doc_of(const std::string &modes, const std::string &week = ON_AT_SIX,
                   const std::string &name = "\"Porch\"") {
  return R"({"name":)" + name + R"(,"modes":)" + modes + R"(,"week":)" + week + "}";
}

// The On mode and a week of `week`.
std::string with_week(const std::string &week) { return doc_of(std::string("[") + ON_MODE + "]", week); }

// One mode, "Day", whose list is `actions`, at 06:00 on Monday.
std::string with_actions(const std::string &actions) {
  return doc_of(R"([{"id":"day","name":"Day","actions":[)" + actions + "]}]",
                R"({"mon":[{"at":"06:00","mode":"day"}]})");
}

// The On mode, then `mode`.
std::string with_mode(const std::string &mode) { return doc_of(std::string("[") + ON_MODE + "," + mode + "]"); }

std::string refusal(const std::string &json, bool require_id = false) {
  ScheduleConfig config;
  std::string error;
  EXPECT_FALSE(from_json(json, &config, &error, require_id)) << json;
  return error;
}

std::string repeated(const std::string &item, int count) {
  std::string list;
  for (int i = 0; i < count; i++)
    list += std::string(i ? "," : "") + item;
  return list;
}

const char *const PRESET_ECO = R"({"type":"thermostat","id":"living-room","value":{"type":"preset","preset":"eco"}})";
const char *const RELAY_ON = R"({"type":"relay","id":"relay_1","value":{"type":"on"}})";

}  // namespace

TEST(ScheduleConfig, TheFileRoundTripsByteForByte) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(GOLDEN, &config, &error, true)) << error;
  EXPECT_EQ(GOLDEN, to_json(config));
  EXPECT_EQ(4u, config.revision);
  ASSERT_EQ(2u, config.modes.size());
  EXPECT_EQ("day", config.modes[0].id);
  EXPECT_STREQ("amber", color_to_string(config.modes[0].color));
  EXPECT_EQ(0, config.modes[0].first);
  EXPECT_EQ(4, config.modes[0].count);
  EXPECT_EQ(4, config.modes[1].first);
  EXPECT_EQ(3, config.modes[1].count);
  // Every target and every key once, the actions holding indexes into them.
  ASSERT_EQ(3u, config.targets.size());
  EXPECT_EQ((Target{TargetType::THERMOSTAT, "living-room"}), config.targets[0]);
  EXPECT_EQ((Target{TargetType::RELAY, "boiler_pump"}), config.targets[1]);
  EXPECT_EQ((Target{TargetType::RELAY, "relay_5"}), config.targets[2]);
  EXPECT_EQ((std::vector<std::string>{"comfort", "eco"}), strings(config.preset_keys));
  ASSERT_EQ(7u, config.actions.size());
  EXPECT_EQ(ActionType::WAIT, config.actions[2].type);
  EXPECT_FLOAT_EQ(120.f, config.actions[2].number);
  EXPECT_EQ(1, config.actions[6].target);
  EXPECT_EQ("eco", config.preset_of(config.actions[4]));
  ASSERT_EQ(2u, config.points.size());
  EXPECT_EQ(1, config.points[1].mode);

  std::string encoded;
  ASSERT_EQ(EncodeError::NONE, config.encode(&encoded));
  EXPECT_EQ(GOLDEN, encoded);
}

// 16 schedules of 8 modes of 12 actions stay in RAM: an action is a few bytes, its target and
// its preset kept once per schedule.
TEST(ScheduleConfig, AnActionIsSmall) {
  EXPECT_LE(sizeof(Action), 8u);
  EXPECT_LE(sizeof(Point), 4u);
}

TEST(ScheduleConfig, EveryValueAndAModeWithoutColourOrActionsRoundTrip) {
  const std::string json = R"({"version":1,"revision":0,"id":"heat","name":"Heat","enabled":false,"modes":[)"
                           R"({"id":"m1","name":"Warm","actions":[)"
                           R"({"type":"thermostat","id":"hall","value":{"type":"on"}},)"
                           R"({"type":"thermostat","id":"hall","value":{"type":"target","target":21.5}},)"
                           R"({"type":"wait","seconds":86400},{"type":"wait","seconds":1},)"
                           R"({"type":"thermostat","id":"hall","value":{"type":"off"}}]},)"
                           R"({"id":"m2","name":"Idle","color":"grey","actions":[]}],)"
                           R"("week":{"mon":[{"at":"00:00","mode":"m1"}],"tue":[],"wed":[{"at":"12:15","mode":"m2"}],)"
                           R"("thu":[],"fri":[],"sat":[],"sun":[{"at":"23:59","mode":"m1"}]}})";
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(json, &config, &error, true)) << error;
  EXPECT_FALSE(config.enabled);
  EXPECT_EQ(0, config.modes[0].color) << "no colour of its own: the editor picks one";
  EXPECT_EQ(0, config.modes[1].count);
  EXPECT_EQ(json, to_json(config));
}

TEST(ScheduleConfig, AMissingDayOrListReadsAsNoneAndIsWrittenEmpty) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(doc_of(R"([{"id":"idle","name":"Idle"}])", R"({"thu":[{"at":"07:00","mode":"idle"}]})"),
                        &config, &error))
      << error;
  ASSERT_EQ(1u, config.points.size());
  EXPECT_EQ(wm(3, 7, 0), config.points[0].minute_of_week);
  const std::string json = to_json(config);
  EXPECT_NE(std::string::npos, json.find(R"("modes":[{"id":"idle","name":"Idle","actions":[]}])")) << json;
  EXPECT_NE(std::string::npos, json.find(R"("week":{"mon":[],"tue":[],"wed":[],"thu":[{"at":"07:00","mode":"idle"}],)"
                                         R"("fri":[],"sat":[],"sun":[]})"));
}

TEST(ScheduleConfig, EachDaysPointsAreSorted) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(with_week(R"({"tue":[{"at":"20:00","mode":"on"},{"at":"08:00","mode":"on"}],)"
                                  R"("mon":[{"at":"23:00","mode":"on"},{"at":"01:00","mode":"on"}]})"),
                        &config, &error))
      << error;
  std::vector<uint16_t> minutes;
  for (const Point &point : config.points)
    minutes.push_back(point.minute_of_week);
  EXPECT_EQ((std::vector<uint16_t>{wm(0, 1, 0), wm(0, 23, 0), wm(1, 8, 0), wm(1, 20, 0)}), minutes);
}

TEST(ScheduleConfig, ATargetAndAPresetKeyAreKeptOnceAcrossModes) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(doc_of(std::string(R"([{"id":"a","name":"A","actions":[)") + PRESET_ECO + "," + RELAY_ON +
                                   R"(]},{"id":"b","name":"B","actions":[)" + RELAY_ON + "," + PRESET_ECO +
                                   R"(,{"type":"relay","id":"relay_2","value":{"type":"off"}}]}])",
                               R"({"mon":[{"at":"06:00","mode":"a"}]})"),
                        &config, &error))
      << error;
  EXPECT_EQ(3u, config.targets.size());
  EXPECT_EQ((std::vector<std::string>{"eco"}), strings(config.preset_keys));
  EXPECT_TRUE(config.names_target(Target{TargetType::RELAY, "relay_2"}));
  EXPECT_FALSE(config.names_target(Target{TargetType::THERMOSTAT, "relay_2"})) << "a type and an id";
  EXPECT_EQ("", config.preset_of(config.actions[1])) << "a relay action names no preset";
}

TEST(ScheduleConfig, AWholeNumberIsATarget) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"target","target":21}})"),
                        &config, &error))
      << error;
  EXPECT_FLOAT_EQ(21.f, config.actions[0].number);
  EXPECT_NE(std::string::npos, to_json(config).find(R"({"type":"target","target":21})"));
}

TEST(ScheduleConfig, NamesAreTrimmedAndEnabledIsTheDefault) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(
      from_json(doc_of(R"([{"id":"on","name":"  Evening \t","actions":[]}])", ON_AT_SIX, "\"  Porch light \\t\""),
                &config, &error))
      << error;
  EXPECT_EQ("Porch light", config.name);
  EXPECT_EQ("Evening", config.modes[0].name);
  EXPECT_TRUE(config.enabled);
  EXPECT_EQ("", config.id) << "a draft needs no id";
  EXPECT_EQ(SCHEDULE_VERSION, config.version);
}

TEST(ScheduleConfig, AModeNameIsCharactersInAnyLanguage) {
  std::string cyrillic;
  for (int i = 0; i < 24; i++)
    cyrillic += "ж";
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(doc_of(R"([{"id":"on","name":")" + cyrillic + R"("}])"), &config, &error)) << error;
  EXPECT_EQ("Mode 1: name is longer than 24 characters",
            refusal(doc_of(R"([{"id":"on","name":")" + cyrillic + R"(ж"}])")));
  // A mode name never goes into a URL.
  ASSERT_TRUE(from_json(doc_of(R"([{"id":"on","name":"In/Out \\ 🔥"}])"), &config, &error)) << error;
}

TEST(ScheduleConfig, AnIdIsReadWhenThereAndRequiredWhenAsked) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(R"({"id":"porch",)" + with_week(ON_AT_SIX).substr(1), &config, &error)) << error;
  EXPECT_EQ("porch", config.id);
  EXPECT_EQ("id is required", refusal(with_week(ON_AT_SIX), true));
  EXPECT_EQ("id must be a slug: lowercase letters, digits and single dashes",
            refusal(R"({"id":"Porch Light",)" + with_week(ON_AT_SIX).substr(1), true));
}

// A long string is refused, or quoted cut short, without being read whole: a name or an id from
// the view it is in the document, a word the sentence quotes at a name's length.
TEST(ScheduleConfig, ALongStringIsRefusedOrQuotedShort) {
  const std::string long_text(16 * 1024, 'x');
  EXPECT_EQ("Name is longer than 48 characters",
            refusal(doc_of(std::string("[") + ON_MODE + "]", ON_AT_SIX, "\"" + long_text + "\"")));
  EXPECT_EQ("id must be a slug: lowercase letters, digits and single dashes",
            refusal(R"({"id":")" + long_text + R"(",)" + with_week(ON_AT_SIX).substr(1), true));
  EXPECT_EQ(
      "id must be a slug: lowercase letters, digits and single dashes",
      refusal(R"({"id":")" + std::string(ID_MAX_LENGTH + 1, 'a') + R"(",)" + with_week(ON_AT_SIX).substr(1), true));
  const std::string shown = "\"" + std::string(NAME_MAX_LENGTH, 'x') + "…\"";
  EXPECT_EQ("Mode \"Day\", action 1: unknown action type " + shown,
            refusal(with_actions(R"({"type":")" + long_text + R"("})")));
  EXPECT_EQ(
      "Mon 06:00: there is no mode " + shown,
      refusal(doc_of(std::string("[") + ON_MODE + "]", R"({"mon":[{"at":"06:00","mode":")" + long_text + R"("}]})")));
  EXPECT_EQ("Unknown day " + shown + ": expected mon, tue, wed, thu, fri, sat or sun",
            refusal(with_week(R"({")" + long_text + R"(":[]})")));
  // A Save's id is only looked up: a long one names no schedule and is not kept whole.
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(R"({"id":")" + long_text + R"(",)" + with_week(ON_AT_SIX).substr(1), &config, &error)) << error;
  EXPECT_EQ(ID_MAX_LENGTH + 1, config.id.size());
}

// Older files read as this version; a newer one keeps its number so the engine never writes it.
TEST(ScheduleConfig, ANewerVersionIsKept) {
  struct Case {
    const char *version;
    uint16_t read;
  };
  for (const Case &c : {Case{"0", 1}, Case{"1", 1}, Case{"\"9\"", 1}, Case{"1.5", 2}, Case{"2", 2}, Case{"7", 7}}) {
    ScheduleConfig config;
    std::string error;
    const std::string json = std::string(R"({"version":)") + c.version + "," + with_week(ON_AT_SIX).substr(1);
    ASSERT_TRUE(from_json(json, &config, &error)) << error;
    EXPECT_EQ(c.read, config.version) << c.version;
    EXPECT_EQ(c.read > SCHEDULE_VERSION, config.from_newer_firmware()) << c.version;
  }
}

// A file from before the revision, or a hand-written one, reads as 0; so does anything that is not
// a whole number fitting 32 bits.
TEST(ScheduleConfig, ARevisionIsAWholeNumberAndZeroWithoutOne) {
  struct Case {
    const char *revision;
    uint32_t read;
  };
  for (const Case &c : {Case{nullptr, 0}, Case{"7", 7}, Case{"4294967295", 4294967295u}, Case{"-1", 0}, Case{"1.5", 0},
                        Case{"\"7\"", 0}, Case{"4294967296", 0}, Case{"null", 0}}) {
    ScheduleConfig config;
    std::string error;
    const std::string json = std::string("{") +
                             (c.revision != nullptr ? std::string(R"("revision":)") + c.revision + "," : "") +
                             with_week(ON_AT_SIX).substr(1);
    ASSERT_TRUE(from_json(json, &config, &error)) << error;
    EXPECT_EQ(c.read, config.revision) << (c.revision != nullptr ? c.revision : "none");
  }
  ScheduleConfig config;
  config.revision = 12;
  EXPECT_NE(std::string::npos, to_json(config).find(R"({"version":1,"revision":12,"id":)"))
      << "written after the version";
}

TEST(ScheduleConfig, EveryBrokenRuleHasItsSentence) {
  const std::string thirteen_points = [] {
    std::string list = "[";
    for (int i = 0; i < 13; i++)
      list += std::string(i ? "," : "") + R"({"at":")" + format_hhmm(i * 60) + R"(","mode":"on"})";
    return list + "]";
  }();
  const std::string nine_modes = [] {
    std::string list = "[";
    for (int i = 0; i < 9; i++)
      list += std::string(i ? "," : "") + R"({"id":"m)" + std::to_string(i) + R"(","name":"M)" + std::to_string(i) +
              R"("})";
    return list + "]";
  }();
  const std::string colors = "color must be one of red, orange, amber, yellow, lime, green, teal, cyan, blue, indigo, "
                             "violet, pink or grey";
  struct Case {
    std::string json;
    std::string error;
  };
  const Case cases[] = {
      // The schedule's name.
      {R"({"modes":[],"week":{}})", "Name is required"},
      {doc_of("[]", "{}", "\"   \""), "Name is required"},
      {doc_of("[]", "{}", "7"), "Name is required"},
      {doc_of("[]", "{}", "\"" + std::string(49, 'x') + "\""), "Name is longer than 48 characters"},
      {doc_of("[]", "{}", "\"A\xff\""), "Name is not valid UTF-8"},
      {doc_of("[]", "{}", "\"A\\u0001B\""), "Name cannot contain control characters"},
      {doc_of("[]", "{}", "\"A\\u007fB\""), "Name cannot contain control characters"},
      {doc_of("[]", "{}", "\"A\\u0085B\""), "Name cannot contain control characters"},
      {doc_of("[]", "{}", "\"In/Out\""), "Name cannot contain '/'"},
      {doc_of("[]", "{}", "\"In\\\\Out\""), "Name cannot contain '\\'"},
      // The modes, one at a time.
      {R"({"name":"Porch","week":{}})", "modes is required"},
      {doc_of("\"on\""), "modes is required"},
      {doc_of("{}"), "modes is required"},
      {doc_of("[]"), "A schedule needs at least one mode"},
      {doc_of(nine_modes), "A schedule has at most 8 modes"},
      {with_mode("\"off\""), "Mode 2: a mode must be an object"},
      {with_mode(R"({"id":"off"})"), "Mode 2: name is required"},
      {with_mode(R"({"id":"off","name":"   "})"), "Mode 2: name is required"},
      {with_mode(R"({"id":"off","name":7})"), "Mode 2: name is required"},
      {with_mode(R"({"id":"off","name":")" + std::string(25, 'x') + R"("})"),
       "Mode 2: name is longer than 24 characters"},
      {with_mode(R"({"id":"off","name":"A)"
                 "\xff"
                 R"("})"),
       "Mode 2: name is not valid UTF-8"},
      {with_mode(R"({"id":"off","name":"A\u0001"})"), "Mode 2: name cannot contain control characters"},
      {with_mode(R"({"id":"off","name":" on "})"), "Two modes are called \"on\""},
      {with_mode(R"({"id":"off","name":"ON"})"), "Two modes are called \"ON\""},
      {with_mode(R"({"name":"Off"})"), "Mode \"Off\": id is required"},
      {with_mode(R"({"id":"","name":"Off"})"), "Mode \"Off\": id is required"},
      {with_mode(R"({"id":5,"name":"Off"})"), "Mode \"Off\": id is required"},
      {with_mode(R"({"id":"Off","name":"Off"})"),
       "Mode \"Off\": id must be at most 24 lowercase letters, digits and dashes"},
      {with_mode(R"({"id":"a_b","name":"Off"})"),
       "Mode \"Off\": id must be at most 24 lowercase letters, digits and dashes"},
      {with_mode(R"({"id":")" + std::string(25, 'a') + R"(","name":"Off"})"),
       "Mode \"Off\": id must be at most 24 lowercase letters, digits and dashes"},
      {with_mode(R"({"id":"on","name":"Off"})"), "Two modes have the id \"on\""},
      {with_mode(R"({"id":"off","name":"Off","color":"purple"})"), "Mode \"Off\": " + colors},
      {with_mode(R"({"id":"off","name":"Off","color":"Red"})"), "Mode \"Off\": " + colors},
      {with_mode(R"({"id":"off","name":"Off","color":""})"), "Mode \"Off\": " + colors},
      {with_mode(R"({"id":"off","name":"Off","color":3})"), "Mode \"Off\": " + colors},
      {with_mode(R"({"id":"off","name":"Off","actions":{}})"), "Mode \"Off\": actions must be a list"},
      {with_mode(R"({"id":"off","name":"Off","actions":"none"})"), "Mode \"Off\": actions must be a list"},
      {with_mode(R"({"id":"off","name":"Off","actions":[)" + repeated(RELAY_ON, 13) + "]}"),
       "Mode \"Off\": at most 12 actions"},
      // Each action: its type, what it drives, then its value.
      {with_actions("\"wait\""), "Mode \"Day\", action 1: an action must be an object"},
      {with_actions("{}"), "Mode \"Day\", action 1: type is required"},
      {with_actions(R"({"type":5})"), "Mode \"Day\", action 1: type is required"},
      {with_actions(R"({"type":""})"), "Mode \"Day\", action 1: type is required"},
      {with_actions(R"({"type":"toggle","id":"relay_1"})"), "Mode \"Day\", action 1: unknown action type \"toggle\""},
      {with_actions(R"({"type":"relay","value":{"type":"on"}})"), "Mode \"Day\", action 1: id is required"},
      {with_actions(R"({"type":"relay","id":"","value":{"type":"on"}})"), "Mode \"Day\", action 1: id is required"},
      {with_actions(R"({"type":"thermostat","id":7,"value":{"type":"on"}})"), "Mode \"Day\", action 1: id is required"},
      {with_actions(R"({"type":"relay","id":"relay_1"})"), "Mode \"Day\", action 1: value is required"},
      {with_actions(R"({"type":"relay","id":"relay_1","value":"on"})"), "Mode \"Day\", action 1: value is required"},
      {with_actions(R"({"type":"relay","id":"relay_1","value":{}})"), "Mode \"Day\", action 1: value is required"},
      {with_actions(R"({"type":"relay","id":"relay_1","value":{"type":"toggle"}})"),
       "Mode \"Day\", action 1: unknown value type \"toggle\""},
      {with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"follow"}})"),
       "Mode \"Day\", action 1: unknown value type \"follow\""},
      {with_actions(R"({"type":"relay","id":"relay_1","value":{"type":"preset","preset":"eco"}})"),
       "Mode \"Day\", action 1: a relay takes on or off"},
      {with_actions(R"({"type":"relay","id":"relay_1","value":{"type":"target","target":20}})"),
       "Mode \"Day\", action 1: a relay takes on or off"},
      {with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"preset"}})"),
       "Mode \"Day\", action 1: preset is required"},
      {with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"preset","preset":""}})"),
       "Mode \"Day\", action 1: preset is required"},
      {with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"preset","preset":"Eco"}})"),
       "Mode \"Day\", action 1: preset must be a preset's key"},
      {with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"preset","preset":"a--b"}})"),
       "Mode \"Day\", action 1: preset must be a preset's key"},
      {with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"preset","preset":")" + std::string(49, 'a') +
                    R"("}})"),
       "Mode \"Day\", action 1: preset must be a preset's key"},
      {with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"target"}})"),
       "Mode \"Day\", action 1: target must be a number"},
      {with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"target","target":"21"}})"),
       "Mode \"Day\", action 1: target must be a number"},
      {with_actions(R"({"type":"thermostat","id":"hall","value":{"type":"target","target":1e300}})"),
       "Mode \"Day\", action 1: target must be a number"},
      {with_actions(R"({"type":"wait"})"), "Mode \"Day\", action 1: seconds must be a whole number"},
      {with_actions(R"({"type":"wait","seconds":"60"})"), "Mode \"Day\", action 1: seconds must be a whole number"},
      {with_actions(R"({"type":"wait","seconds":1.5})"), "Mode \"Day\", action 1: seconds must be a whole number"},
      {with_actions(R"({"type":"wait","seconds":0})"), "Mode \"Day\", action 1: a wait is 1 s to 24 h"},
      {with_actions(R"({"type":"wait","seconds":-60})"), "Mode \"Day\", action 1: a wait is 1 s to 24 h"},
      {with_actions(R"({"type":"wait","seconds":86401})"), "Mode \"Day\", action 1: a wait is 1 s to 24 h"},
      {with_actions(R"({"type":"wait","seconds":1e300})"), "Mode \"Day\", action 1: a wait is 1 s to 24 h"},
      // In the list's order, and mode by mode.
      {with_actions(std::string(RELAY_ON) + R"(,{"type":"wait","seconds":0},{"type":"toggle"})"),
       "Mode \"Day\", action 2: a wait is 1 s to 24 h"},
      {doc_of(R"([{"id":"a","name":"A","actions":[{"type":"wait"}]},{"id":"","name":"B"}])"),
       "Mode \"A\", action 1: seconds must be a whole number"},
      {doc_of(R"([{"id":"Bad","name":""}])"), "Mode 1: name is required"},
      // The week, a day at a time.
      {doc_of(std::string("[") + ON_MODE + "]", "null"), "week is required"},
      {with_week("[]"), "week is required"},
      {with_week(R"({"mo":[]})"), "Unknown day \"mo\": expected mon, tue, wed, thu, fri, sat or sun"},
      {with_week(R"({"mon":[],"Tue":[]})"), "Unknown day \"Tue\": expected mon, tue, wed, thu, fri, sat or sun"},
      {with_week(R"({"wed":{}})"), "Wed: switch points must be a list"},
      {with_week(R"({"mon":)" + thirteen_points + "}"), "Mon: at most 12 switch points a day"},
      {with_week(R"({"mon":[{"mode":"on"}]})"), "Mon: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"sat":[{"at":"24:00","mode":"on"}]})"), "Sat: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"mon":[{"at":"7:00","mode":"on"}]})"), "Mon: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"mon":[{"at":700,"mode":"on"}]})"), "Mon: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"mon":["06:30"]})"), "Mon: a switch point must be an object"},
      {with_week(R"({"mon":[{"at":"06:30","mode":"on"},{"at":"06:30","mode":"on"}]})"), "Mon: 06:30 is there twice"},
      // Each point in the body's order: its time, whether the day has it already, then its mode.
      {with_week(R"({"mon":[{"at":"06:30","mode":"on"},{"at":"06:30","mode":"dusk"}]})"), "Mon: 06:30 is there twice"},
      {with_week(R"({"mon":[{"at":"07:00","mode":"dusk"},{"at":"07:00","mode":"on"}]})"),
       "Mon 07:00: there is no mode \"dusk\""},
      {with_week(R"({"mon":[{"at":"07:00","mode":"on"},{"at":"6:30","mode":"dusk"}]})"),
       "Mon: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"mon":[{"at":"06:30"}]})"), "Mon 06:30: mode is required"},
      {with_week(R"({"mon":[{"at":"06:30","mode":""}]})"), "Mon 06:30: mode is required"},
      {with_week(R"({"mon":[{"at":"06:30","mode":1}]})"), "Mon 06:30: mode is required"},
      {with_week(R"({"tue":[{"at":"06:30","mode":"On"}]})"), "Tue 06:30: there is no mode \"On\""},
      {with_week("{}"), "A schedule needs at least one switch point"},
      {with_week(R"({"mon":[],"sun":[]})"), "A schedule needs at least one switch point"},
  };
  for (const Case &c : cases)
    EXPECT_EQ(c.error, refusal(c.json)) << c.json;
}

TEST(ScheduleConfig, ANullDocumentIsRefused) {
  ScheduleConfig config;
  std::string error;
  EXPECT_FALSE(config.deserialize(JsonObjectConst(), false, &error));
  EXPECT_EQ("document is not an object", error);
}

// Built in C++, the same rules hold, and what no file can say is refused too.
TEST(ScheduleConfig, ValidateHoldsADocumentBuiltInCppToTheSameRules) {
  struct Case {
    ScheduleConfig config;
    std::string error;
  };
  auto one_mode = [](const char *id, const char *name) {
    ScheduleConfig c;
    c.name = "Porch";
    c.add_mode(id, name);
    c.add_point(0, 0);
    return c;
  };
  ScheduleConfig no_modes;
  no_modes.name = "Porch";
  ScheduleConfig nine_modes = one_mode("m0", "M0");
  for (int i = 1; i < 9; i++)
    nine_modes.add_mode("m" + std::to_string(i), "M" + std::to_string(i));
  ScheduleConfig same_name = one_mode("a", "Day");
  same_name.add_mode("b", "day");
  ScheduleConfig same_id = one_mode("a", "Day");
  same_id.add_mode("a", "Night");
  ScheduleConfig bad_color = one_mode("a", "Day");
  bad_color.modes[0].color = COLOR_COUNT + 1;
  ScheduleConfig thirteen = one_mode("a", "Day");
  for (int i = 0; i < 13; i++)
    thirteen.add_relay("relay_1", true);
  ScheduleConfig outside = one_mode("a", "Day");
  outside.modes[0].count = 1;
  ScheduleConfig relay_preset = one_mode("a", "Day");
  relay_preset.add_relay("relay_1", true);
  relay_preset.actions[0].value = ValueType::PRESET;
  ScheduleConfig wrong_table = one_mode("a", "Day");
  wrong_table.add_relay("relay_1", true);
  wrong_table.actions[0].type = ActionType::THERMOSTAT;
  ScheduleConfig lost_target = one_mode("a", "Day");
  lost_target.add_relay("relay_1", true);
  lost_target.targets.clear();
  ScheduleConfig lost_key = one_mode("a", "Day");
  lost_key.add_thermostat("hall", ValueType::PRESET, "eco");
  lost_key.preset_keys.clear();
  ScheduleConfig bad_key = one_mode("a", "Day");
  bad_key.add_thermostat("hall", ValueType::PRESET, "Eco");
  ScheduleConfig no_target = one_mode("a", "Day");
  no_target.add_thermostat("hall", ValueType::TARGET, "", INFINITY);
  ScheduleConfig long_wait = one_mode("a", "Day");
  long_wait.add_wait(MAX_WAIT_SECONDS + 1);
  ScheduleConfig no_wait = one_mode("a", "Day");
  no_wait.add_wait(0);
  ScheduleConfig half_wait = one_mode("a", "Day");
  half_wait.add_wait(1);
  half_wait.actions[0].number = 1.5f;
  ScheduleConfig no_points = one_mode("a", "Day");
  no_points.points.clear();
  ScheduleConfig past_the_week = one_mode("a", "Day");
  past_the_week.points[0].minute_of_week = 7 * 1440;
  ScheduleConfig thirteen_points = one_mode("a", "Day");
  thirteen_points.points.clear();
  for (int i = 0; i < 13; i++)
    thirteen_points.add_point(wm(2, i, 0), 0);
  ScheduleConfig twice = one_mode("a", "Day");
  twice.add_point(0, 0);
  ScheduleConfig no_mode = one_mode("a", "Day");
  no_mode.points[0].mode = 1;
  const Case cases[] = {
      {one_mode("a", ""), "Mode 1: name is required"},
      {no_modes, "A schedule needs at least one mode"},
      {nine_modes, "A schedule has at most 8 modes"},
      {same_name, "Two modes are called \"day\""},
      {one_mode("", "Day"), "Mode \"Day\": id is required"},
      {one_mode("A", "Day"), "Mode \"Day\": id must be at most 24 lowercase letters, digits and dashes"},
      {same_id, "Two modes have the id \"a\""},
      {bad_color,
       "Mode \"Day\": color must be one of red, orange, amber, yellow, lime, green, teal, cyan, blue, indigo, violet, "
       "pink or grey"},
      {thirteen, "Mode \"Day\": at most 12 actions"},
      {outside, "Mode \"Day\": actions must be a list"},
      {relay_preset, "Mode \"Day\", action 1: a relay takes on or off"},
      {wrong_table, "Mode \"Day\", action 1: id is required"},
      {lost_target, "Mode \"Day\", action 1: id is required"},
      {lost_key, "Mode \"Day\", action 1: preset is required"},
      {bad_key, "Mode \"Day\", action 1: preset must be a preset's key"},
      {no_target, "Mode \"Day\", action 1: target must be a number"},
      {long_wait, "Mode \"Day\", action 1: a wait is 1 s to 24 h"},
      {no_wait, "Mode \"Day\", action 1: a wait is 1 s to 24 h"},
      {half_wait, "Mode \"Day\", action 1: seconds must be a whole number"},
      {no_points, "A schedule needs at least one switch point"},
      {past_the_week, "A switch point is past Sun 23:59"},
      {thirteen_points, "Wed: at most 12 switch points a day"},
      {twice, "Mon: 00:00 is there twice"},
      {no_mode, "Mon 00:00: mode is required"},
  };
  for (const Case &c : cases) {
    std::string error;
    EXPECT_FALSE(c.config.validate(&error)) << c.error;
    EXPECT_EQ(c.error, error);
  }
  ScheduleConfig empty_name = one_mode("a", "Day");
  empty_name.name = "";
  std::string error;
  EXPECT_FALSE(empty_name.validate(&error));
  EXPECT_EQ("Name is required", error) << "the schedule's name first";
  EXPECT_TRUE(daily().validate(&error)) << error;
  EXPECT_TRUE(home().validate(&error)) << error;
  EXPECT_TRUE(thermostat_doc("Heat", "living-room",
                             {preset_at(wm(0, 6, 0), "eco"), off_at(wm(0, 22, 0)), target_at(wm(1, 6, 0), 21.5f)})
                  .validate(&error))
      << error;
}

TEST(ScheduleConfig, AddPointKeepsTheListSortedAndSortPointsRestoresIt) {
  ScheduleConfig config = relay_doc("Porch", "relay_1", {off_at(wm(2, 0, 0)), on_at(wm(0, 0, 0)), on_at(wm(1, 0, 0))});
  EXPECT_EQ(wm(0, 0, 0), config.points[0].minute_of_week);
  EXPECT_EQ(wm(2, 0, 0), config.points[2].minute_of_week);
  std::swap(config.points[0], config.points[2]);
  config.sort_points();
  EXPECT_EQ(wm(0, 0, 0), config.points[0].minute_of_week);
  EXPECT_EQ(wm(2, 0, 0), config.points[2].minute_of_week);
}

TEST(ScheduleConfig, WriteActionIsTheActionAsTheFileHoldsIt) {
  ScheduleConfig config;
  config.add_mode("a", "A");
  config.add_thermostat("hall", ValueType::PRESET, "eco");
  config.add_thermostat("hall", ValueType::OFF);
  config.add_thermostat("hall", ValueType::ON);
  config.add_thermostat("hall", ValueType::TARGET, "", 19.5f);
  config.add_relay("relay_1", false);
  config.add_wait(90);
  std::vector<std::string> written;
  for (const Action &action : config.actions) {
    JsonDocument doc;
    config.write_action(doc.to<JsonObject>(), action);
    std::string json;
    serializeJson(doc, json);
    written.push_back(json);
  }
  EXPECT_EQ((std::vector<std::string>{R"({"type":"thermostat","id":"hall","value":{"type":"preset","preset":"eco"}})",
                                      R"({"type":"thermostat","id":"hall","value":{"type":"off"}})",
                                      R"({"type":"thermostat","id":"hall","value":{"type":"on"}})",
                                      R"({"type":"thermostat","id":"hall","value":{"type":"target","target":19.5}})",
                                      R"({"type":"relay","id":"relay_1","value":{"type":"off"}})",
                                      R"({"type":"wait","seconds":90})"}),
            written);
  EXPECT_EQ(6, config.modes[0].count);
  EXPECT_EQ(2u, config.targets.size());
  EXPECT_STREQ("on", value_type_to_string(ValueType::ON));
  EXPECT_STREQ("relay", target_type_to_string(TargetType::RELAY));
  EXPECT_STREQ("thermostat", target_type_to_string(TargetType::THERMOSTAT));
  EXPECT_STREQ("wait", action_type_to_string(ActionType::WAIT));
  EXPECT_STREQ("", color_to_string(0));
  EXPECT_STREQ("red", color_to_string(1));
  EXPECT_STREQ("grey", color_to_string(COLOR_COUNT));
  EXPECT_STREQ("", color_to_string(COLOR_COUNT + 1));
  EXPECT_EQ(-1, config.mode_index("b"));
}

TEST(ScheduleConfig, EncodeRefusesWhatTheNextBootCouldNotLoad) {
  const ScheduleConfig config = daily();
  std::string json = "untouched";
  EXPECT_EQ(EncodeError::TOO_LARGE, config.encode(&json, 100));
  EXPECT_EQ("untouched", json);

  // A heap that runs out mid-document: what is left would still serialize, with keys missing.
  struct Refusing : ArduinoJson::Allocator {
    void *allocate(size_t) override { return nullptr; }
    void deallocate(void *ptr) override { free(ptr); }
    void *reallocate(void *, size_t) override { return nullptr; }
  } refusing;
  EXPECT_EQ(EncodeError::NO_MEMORY, config.encode(&json, MAX_FILE_BYTES, &refusing));
  EXPECT_EQ("untouched", json);
}

// Every mode and every action the limits allow, every point too, with ids of a usual length:
// the file still fits under its cap.
TEST(ScheduleConfig, EightFullModesAndAFullWeekFitTheFile) {
  ScheduleConfig config;
  config.name = std::string(NAME_MAX_LENGTH, 'x');
  for (size_t m = 0; m < MAX_MODES; m++) {
    config.add_mode("mode-" + std::to_string(m), std::string(MODE_NAME_MAX_LENGTH - 1, 'y') + std::to_string(m), 13);
    for (size_t a = 0; a < MAX_ACTIONS; a++)
      config.add_thermostat("thermostat-" + std::to_string(a), ValueType::PRESET, "preset-" + std::to_string(m));
  }
  for (int day = 0; day < 7; day++) {
    for (int p = 0; p < 12; p++)
      config.add_point(wm(day, p, 0), static_cast<uint8_t>(p % MAX_MODES));
  }
  std::string error;
  ASSERT_TRUE(config.validate(&error)) << error;
  EXPECT_EQ(12u, config.targets.size());
  EXPECT_EQ(8u, config.preset_keys.size());
  std::string json;
  EXPECT_EQ(EncodeError::NONE, config.encode(&json));
}

// --- Names and ids ---

TEST(Names, TheLimitIsCharactersNotBytes) {
  std::string error;
  std::string cyrillic;
  for (int i = 0; i < 48; i++)
    cyrillic += "ж";
  EXPECT_EQ(96u, cyrillic.size());
  EXPECT_TRUE(validate_name(cyrillic, &error)) << error;
  EXPECT_FALSE(validate_name(cyrillic + "ж", &error));
  EXPECT_EQ("Name is longer than 48 characters", error);
  EXPECT_TRUE(validate_name("Котёл 🔥 утро", &error)) << error;
}

TEST(Names, AnythingButUtf8IsRefused) {
  for (const char *name :
       {"\x80", "A\xc3", "\xc0\xaf", "\xe0\x80\xaf", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xf8\x88", "\xe2\x82"}) {
    std::string error;
    EXPECT_FALSE(validate_name(name, &error)) << name;
    EXPECT_EQ("Name is not valid UTF-8", error);
  }
}

TEST(Names, TrimAndKey) {
  EXPECT_EQ("Porch light", trim_name(" \t Porch light \n"));
  EXPECT_EQ("", trim_name("   "));
  EXPECT_EQ(name_key("Porch  Light"), name_key("porch light"));
  EXPECT_EQ("котёл утро", name_key("котёл   утро")) << "only ASCII is folded";
  EXPECT_NE(name_key("Котёл"), name_key("котёл"));
}

TEST(Names, CutCharsNeverSplitsACharacter) {
  EXPECT_EQ("жж", cut_chars("жжж", 2));
  EXPECT_EQ("abc", cut_chars("abc", 5));
  EXPECT_EQ("a", cut_chars("a\xff", 5)) << "stops at what is not UTF-8";
}

TEST(Ids, AreMadeFromTheName) {
  EXPECT_EQ("living-room-week", slugify_id("Living room week"));
  EXPECT_EQ("a-b", slugify_id("--A__b--"));
  EXPECT_EQ("schedule", slugify_id("Утро"));
  EXPECT_EQ("kitchen", slugify_id("Утро kitchen"));
  EXPECT_EQ(48u, slugify_id(std::string(60, 'a')).size());
  EXPECT_EQ(std::string(47, 'a'), slugify_id(std::string(47, 'a') + " b")) << "never ends on a dash";
}

TEST(Ids, ASuffixKeepsTheIdInsideTheLimit) {
  EXPECT_EQ("porch", id_with_suffix("porch", 1));
  EXPECT_EQ("porch-2", id_with_suffix("porch", 2));
  const std::string full(48, 'a');
  EXPECT_EQ(std::string(46, 'a') + "-2", id_with_suffix(full, 2));
  EXPECT_EQ(std::string(44, 'a') + "-999", id_with_suffix(full, 999));
  EXPECT_EQ(std::string(44, 'a') + "-12", id_with_suffix(std::string(44, 'a') + "-bbb", 12)) << "never a double dash";
  EXPECT_EQ("schedule-2", id_with_suffix("-", 2));
}

TEST(Ids, AnIdFromOutsideIsASlug) {
  std::string error;
  EXPECT_TRUE(validate_id("living-room-2", &error));
  for (const char *id : {"Living", "a b", "a--b", "-a", "a-", "a/b", "../x"}) {
    EXPECT_FALSE(validate_id(id, &error)) << id;
    EXPECT_EQ("id must be a slug: lowercase letters, digits and single dashes", error);
  }
  EXPECT_FALSE(validate_id("", &error));
  EXPECT_EQ("id is required", error);
}

}  // namespace esphome::schedules::testing
