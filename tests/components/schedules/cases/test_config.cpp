// The document: its file format byte for byte, every rule it is refused for and with which
// sentence, names, ids and the version.
#include "common.h"

namespace esphome::schedules::testing {

namespace {

const char *const GOLDEN = R"({"version":1,"id":"living-room-week","name":"Living room week","enabled":true,)"
                           R"("target":{"type":"thermostat","id":"living-room"},)"
                           R"("week":{"mon":[{"at":"06:30","value":{"type":"preset","preset":"comfort"}},)"
                           R"({"at":"08:30","value":{"type":"preset","preset":"eco"}},)"
                           R"({"at":"17:00","value":{"type":"target","target":21.5}},)"
                           R"({"at":"23:00","value":{"type":"off"}}],)"
                           R"("tue":[],"wed":[],"thu":[],"fri":[],"sat":[],"sun":[]}})";

// A document around `week` and `target`, the rest valid.
std::string with_week(const std::string &week, const char *target = R"({"type":"relay","id":"relay_1"})",
                      const std::string &name = "\"Porch\"") {
  return R"({"name":)" + name + R"(,"target":)" + target + R"(,"week":)" + week + "}";
}

std::string refusal(const std::string &json, bool require_id = false) {
  ScheduleConfig config;
  std::string error;
  EXPECT_FALSE(from_json(json, &config, &error, require_id)) << json;
  return error;
}

const char *const THERMOSTAT = R"({"type":"thermostat","id":"living-room"})";

}  // namespace

TEST(ScheduleConfig, TheFileRoundTripsByteForByte) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(GOLDEN, &config, &error, true)) << error;
  EXPECT_EQ(GOLDEN, to_json(config));
  EXPECT_EQ(TargetType::THERMOSTAT, config.target_type);
  EXPECT_EQ("living-room", config.target_id);
  ASSERT_EQ(4u, config.points.size());
  EXPECT_EQ((std::vector<std::string>{"comfort", "eco"}), config.preset_keys);
  EXPECT_EQ(ValueType::TARGET, config.points[2].type);
  EXPECT_FLOAT_EQ(21.5f, config.points[2].target);

  std::string encoded;
  ASSERT_EQ(EncodeError::NONE, config.encode(&encoded));
  EXPECT_EQ(GOLDEN, encoded);
}

TEST(ScheduleConfig, ARelayWeekRoundTrips) {
  const std::string json =
      R"({"version":1,"id":"porch","name":"Porch","enabled":false,"target":{"type":"relay","id":"relay_1"},)"
      R"("week":{"mon":[{"at":"00:00","value":{"type":"on"}}],"tue":[],"wed":[{"at":"12:15","value":{"type":"off"}}],)"
      R"("thu":[],"fri":[],"sat":[],"sun":[{"at":"23:59","value":{"type":"on"}}]}})";
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(json, &config, &error, true)) << error;
  EXPECT_FALSE(config.enabled);
  EXPECT_EQ(json, to_json(config));
}

TEST(ScheduleConfig, AMissingDayReadsAsNoneAndIsWrittenEmpty) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(with_week(R"({"thu":[{"at":"07:00","value":{"type":"on"}}]})"), &config, &error)) << error;
  ASSERT_EQ(1u, config.points.size());
  EXPECT_EQ(wm(3, 7, 0), config.points[0].minute_of_week);
  EXPECT_NE(std::string::npos,
            to_json(config).find(R"("week":{"mon":[],"tue":[],"wed":[],"thu":[{"at":"07:00","value":{"type":"on"}}],)"
                                 R"("fri":[],"sat":[],"sun":[]})"));
}

TEST(ScheduleConfig, EachDaysPointsAreSorted) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(
      from_json(with_week(R"({"tue":[{"at":"20:00","value":{"type":"off"}},{"at":"08:00","value":{"type":"on"}}],)"
                          R"("mon":[{"at":"23:00","value":{"type":"on"}},{"at":"01:00","value":{"type":"off"}}]})"),
                &config, &error))
      << error;
  std::vector<uint16_t> minutes;
  for (const Point &point : config.points)
    minutes.push_back(point.minute_of_week);
  EXPECT_EQ((std::vector<uint16_t>{wm(0, 1, 0), wm(0, 23, 0), wm(1, 8, 0), wm(1, 20, 0)}), minutes);
}

TEST(ScheduleConfig, APresetKeyIsKeptOnce) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(with_week(R"({"mon":[{"at":"06:00","value":{"type":"preset","preset":"eco"}},)"
                                  R"({"at":"07:00","value":{"type":"preset","preset":"comfort"}},)"
                                  R"({"at":"08:00","value":{"type":"preset","preset":"eco"}}]})",
                                  THERMOSTAT),
                        &config, &error))
      << error;
  EXPECT_EQ((std::vector<std::string>{"eco", "comfort"}), config.preset_keys);
  EXPECT_EQ("eco", config.preset_of(config.points[2]));
  EXPECT_EQ("", config.preset_of(Point{})) << "an off point names no preset";
}

TEST(ScheduleConfig, AWholeNumberIsATarget) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(with_week(R"({"mon":[{"at":"06:00","value":{"type":"target","target":21}}]})", THERMOSTAT),
                        &config, &error))
      << error;
  EXPECT_FLOAT_EQ(21.f, config.points[0].target);
  EXPECT_NE(std::string::npos, to_json(config).find(R"({"type":"target","target":21})"));
}

TEST(ScheduleConfig, TheNameIsTrimmedAndEnabledIsTheDefault) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(with_week(R"({"mon":[{"at":"06:00","value":{"type":"on"}}]})", R"({"type":"relay","id":"r"})",
                                  "\"  Porch light \\t\""),
                        &config, &error))
      << error;
  EXPECT_EQ("Porch light", config.name);
  EXPECT_TRUE(config.enabled);
  EXPECT_EQ("", config.id) << "a draft needs no id";
  EXPECT_EQ(SCHEDULE_VERSION, config.version);
}

TEST(ScheduleConfig, AnIdIsReadWhenThereAndRequiredWhenAsked) {
  ScheduleConfig config;
  std::string error;
  ASSERT_TRUE(from_json(R"({"id":"porch","name":"Porch","target":{"type":"relay","id":"relay_1"},)"
                        R"("week":{"mon":[{"at":"06:00","value":{"type":"on"}}]}})",
                        &config, &error))
      << error;
  EXPECT_EQ("porch", config.id);
  EXPECT_EQ("id is required", refusal(with_week(R"({"mon":[{"at":"06:00","value":{"type":"on"}}]})"), true));
  EXPECT_EQ("id must be a slug: lowercase letters, digits and single dashes",
            refusal(R"({"id":"Porch Light","name":"Porch","target":{"type":"relay","id":"relay_1"},)"
                    R"("week":{"mon":[{"at":"06:00","value":{"type":"on"}}]}})",
                    true));
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
    const std::string json = std::string(R"({"version":)") + c.version +
                             R"(,"name":"Porch","target":{"type":"relay","id":"relay_1"},)"
                             R"("week":{"mon":[{"at":"06:00","value":{"type":"on"}}]}})";
    ASSERT_TRUE(from_json(json, &config, &error)) << error;
    EXPECT_EQ(c.read, config.version) << c.version;
    EXPECT_EQ(c.read > SCHEDULE_VERSION, config.from_newer_firmware()) << c.version;
  }
}

TEST(ScheduleConfig, EveryBrokenRuleHasItsSentence) {
  const std::string thirteen = [] {
    std::string list = "[";
    for (int i = 0; i < 13; i++)
      list += std::string(i ? "," : "") + R"({"at":")" + format_hhmm(i * 60) + R"(","value":{"type":"on"}})";
    return list + "]";
  }();
  struct Case {
    std::string json;
    const char *error;
  };
  const Case cases[] = {
      {R"({"target":{"type":"relay","id":"relay_1"},"week":{}})", "Name is required"},
      {with_week("{}", R"({"type":"relay","id":"r"})", "\"   \""), "Name is required"},
      {with_week("{}", R"({"type":"relay","id":"r"})", "7"), "Name is required"},
      {with_week("{}", R"({"type":"relay","id":"r"})", "\"" + std::string(49, 'x') + "\""),
       "Name is longer than 48 characters"},
      {with_week("{}", R"({"type":"relay","id":"r"})", "\"A\xff\""), "Name is not valid UTF-8"},
      {with_week("{}", R"({"type":"relay","id":"r"})", "\"A\\u0001B\""), "Name cannot contain control characters"},
      {with_week("{}", R"({"type":"relay","id":"r"})", "\"A\\u007fB\""), "Name cannot contain control characters"},
      {with_week("{}", R"({"type":"relay","id":"r"})", "\"A\\u0085B\""), "Name cannot contain control characters"},
      {with_week("{}", R"({"type":"relay","id":"r"})", "\"In/Out\""), "Name cannot contain '/'"},
      {with_week("{}", R"({"type":"relay","id":"r"})", "\"In\\\\Out\""), "Name cannot contain '\\'"},
      {R"({"name":"Porch","week":{}})", "target is required"},
      {R"({"name":"Porch","target":"relay_1","week":{}})", "target is required"},
      {with_week("{}", R"({"id":"relay_1"})"), "target.type must be relay or thermostat"},
      {with_week("{}", R"({"type":"lamp","id":"relay_1"})"), "target.type must be relay or thermostat"},
      {with_week("{}", R"({"type":"relay"})"), "target.id is required"},
      {with_week("{}", R"({"type":"relay","id":""})"), "target.id is required"},
      {with_week("{}", R"({"type":"thermostat","id":5})"), "target.id is required"},
      {R"({"name":"Porch","target":{"type":"relay","id":"relay_1"}})", "week is required"},
      {with_week("[]"), "week is required"},
      {with_week(R"({"mo":[]})"), "Unknown day \"mo\": expected mon, tue, wed, thu, fri, sat or sun"},
      {with_week(R"({"mon":[],"Tue":[]})"), "Unknown day \"Tue\": expected mon, tue, wed, thu, fri, sat or sun"},
      {with_week(R"({"wed":{}})"), "Wed must be a list"},
      {with_week(R"({"mon":)" + thirteen + "}"), "Mon: at most 12 switch points a day"},
      {with_week(R"({"mon":[{"value":{"type":"on"}}]})"), "Mon: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"sat":[{"at":"24:00","value":{"type":"on"}}]})"), "Sat: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"mon":[{"at":"7:00","value":{"type":"on"}}]})"), "Mon: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"mon":[{"at":700,"value":{"type":"on"}}]})"), "Mon: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"mon":["06:30"]})"), "Mon: at must be HH:MM from 00:00 to 23:59"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"on"}},{"at":"06:30","value":{"type":"off"}}]})"),
       "Mon: 06:30 is there twice"},
      {with_week("{}"), "A schedule needs at least one switch point"},
      {with_week(R"({"mon":[],"sun":[]})"), "A schedule needs at least one switch point"},
      {with_week(R"({"mon":[{"at":"06:30"}]})"), "Mon 06:30: value is required"},
      {with_week(R"({"mon":[{"at":"06:30","value":"on"}]})"), "Mon 06:30: value is required"},
      {with_week(R"({"mon":[{"at":"06:30","value":{}}]})"), "Mon 06:30: value.type is required"},
      {with_week(R"({"tue":[{"at":"06:30","value":{"type":"heat"}}]})"), "Tue 06:30: unknown value type \"heat\""},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"preset","preset":"eco"}}]})"),
       "Mon 06:30: a relay takes on or off"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"target","target":20}}]})"),
       "Mon 06:30: a relay takes on or off"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"on"}}]})", THERMOSTAT),
       "Mon 06:30: a thermostat takes a preset, off or a target"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"preset"}}]})", THERMOSTAT), "Mon 06:30: preset is required"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"preset","preset":""}}]})", THERMOSTAT),
       "Mon 06:30: preset is required"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"preset","preset":"Eco"}}]})", THERMOSTAT),
       "Mon 06:30: preset must be a preset's key"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"preset","preset":"a--b"}}]})", THERMOSTAT),
       "Mon 06:30: preset must be a preset's key"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"preset","preset":")" + std::string(49, 'a') + R"("}}]})",
                 THERMOSTAT),
       "Mon 06:30: preset must be a preset's key"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"target"}}]})", THERMOSTAT),
       "Mon 06:30: target must be a number"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"target","target":"21"}}]})", THERMOSTAT),
       "Mon 06:30: target must be a number"},
      {with_week(R"({"mon":[{"at":"06:30","value":{"type":"target","target":1e300}}]})", THERMOSTAT),
       "Mon 06:30: target must be a number"},
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
    const char *error;
  };
  ScheduleConfig thirteen = relay_doc("Porch", "relay_1", {});
  for (int i = 0; i < 13; i++)
    thirteen.add_point(wm(2, i, 0), ValueType::ON);
  ScheduleConfig past_the_week = relay_doc("Porch", "relay_1", {on_at(wm(0, 1, 0))});
  past_the_week.points[0].minute_of_week = 7 * 1440;
  ScheduleConfig lost_key = thermostat_doc("Heat", "living-room", {preset_at(wm(0, 6, 0), "eco")});
  lost_key.preset_keys.clear();
  const Case cases[] = {
      {relay_doc("", "relay_1", {on_at(0)}), "Name is required"},
      {relay_doc("Porch", "", {on_at(0)}), "target.id is required"},
      {relay_doc("Porch", "relay_1", {}), "A schedule needs at least one switch point"},
      {past_the_week, "A switch point is past Sun 23:59"},
      {thirteen, "Wed: at most 12 switch points a day"},
      {relay_doc("Porch", "relay_1", {on_at(wm(4, 6, 30)), off_at(wm(4, 6, 30))}), "Fri: 06:30 is there twice"},
      {relay_doc("Porch", "relay_1", {preset_at(wm(0, 6, 0), "eco")}), "Mon 06:00: a relay takes on or off"},
      {thermostat_doc("Heat", "living-room", {on_at(wm(0, 6, 0))}),
       "Mon 06:00: a thermostat takes a preset, off or a target"},
      {thermostat_doc("Heat", "living-room", {preset_at(wm(0, 6, 0), "")}), "Mon 06:00: preset is required"},
      {lost_key, "Mon 06:00: preset is required"},
      {thermostat_doc("Heat", "living-room", {preset_at(wm(0, 6, 0), "Eco")}),
       "Mon 06:00: preset must be a preset's key"},
      {thermostat_doc("Heat", "living-room", {target_at(wm(0, 6, 0), NAN)}), "Mon 06:00: target must be a number"},
      {thermostat_doc("Heat", "living-room", {target_at(wm(0, 6, 0), INFINITY)}), "Mon 06:00: target must be a number"},
  };
  for (const Case &c : cases) {
    std::string error;
    EXPECT_FALSE(c.config.validate(&error)) << c.error;
    EXPECT_EQ(c.error, error);
  }
  std::string error;
  EXPECT_TRUE(daily().validate(&error)) << error;
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

TEST(ScheduleConfig, WriteValueIsThePointAsTheFileHoldsIt) {
  const ScheduleConfig config = thermostat_doc(
      "Heat", "living-room", {preset_at(wm(0, 6, 0), "eco"), off_at(wm(0, 7, 0)), target_at(wm(0, 8, 0), 19.5f)});
  std::vector<std::string> written;
  for (const Point &point : config.points) {
    JsonDocument doc;
    config.write_value(doc.to<JsonObject>(), point);
    std::string json;
    serializeJson(doc, json);
    written.push_back(json);
  }
  EXPECT_EQ((std::vector<std::string>{R"({"type":"preset","preset":"eco"})", R"({"type":"off"})",
                                      R"({"type":"target","target":19.5})"}),
            written);
  EXPECT_STREQ("on", value_type_to_string(ValueType::ON));
  EXPECT_STREQ("relay", target_type_to_string(TargetType::RELAY));
  EXPECT_STREQ("thermostat", target_type_to_string(TargetType::THERMOSTAT));
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
