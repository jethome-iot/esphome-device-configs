// The stored document. Its key order and names are a flash format: a thermostat written by one
// build must be readable by the next, so the golden below is asserted byte for byte both ways.
#include "common.h"
#include "esphome/components/climate_hub/config_store.h"
#include "esphome/components/climate_hub/param_table.h"

namespace esphome::climate_hub::testing {
namespace {

ClimateConfig sample() {
  ClimateConfig c;
  c.id = "boiler";
  c.name = "Boiler";
  c.kind = ControlKind::PID;
  c.sensor_id = "room_temp";
  c.heat.relay_id = "relay_1";
  c.mode = HubMode::HEAT;
  return c;
}

const char *const GOLDEN =
    R"({"version":1,"id":"boiler","name":"Boiler","enabled":true,"kind":"pid","sensor_id":"room_temp",)"
    R"("update_interval_s":30,"heat":{"relay_id":"relay_1","period_s":300,"min_on_s":10,"min_off_s":10},)"
    R"("cool":{"relay_id":"","period_s":300,"min_on_s":10,"min_off_s":10},)"
    R"("visual":{"min_temperature":5,"max_temperature":45,"step":0.5},)"
    R"("safety":{"sensor_timeout_s":300,"max_temperature":60},)"
    R"("pid":{"kp":0.6,"ki":0.0025,"kd":0,"min_integral":-1,"max_integral":1,"starting_integral_term":0,)"
    R"("output_samples":1,"derivative_samples":8,"deadband_threshold_low":0,"deadband_threshold_high":0,)"
    R"("deadband_kp_multiplier":0,"deadband_ki_multiplier":0,"deadband_kd_multiplier":0,)"
    R"("deadband_output_samples":1},"bang_bang":{"below":0.5,"above":0.5},"mode":"heat",)"
    R"("setpoint":21})";

}  // namespace

TEST(ClimateConfigJson, SerialisesToTheGoldenDocument) { EXPECT_EQ(GOLDEN, to_json(sample())); }

TEST(ClimateConfigJson, GoldenDocumentRoundTrips) {
  ClimateConfig parsed;
  std::string error;
  ASSERT_TRUE(from_json(GOLDEN, &parsed, &error)) << error;
  EXPECT_EQ(GOLDEN, to_json(parsed));
}

// The struct, the parameter table and therefore the form all start a relay's dwell at 10 s.
TEST(ClimateConfigJson, TheDwellDefaultsAreTenSecondsEverywhere) {
  ClimateConfig config;
  EXPECT_FLOAT_EQ(10.f, config.heat.min_on_s);
  EXPECT_FLOAT_EQ(10.f, config.heat.min_off_s);
  EXPECT_FLOAT_EQ(10.f, config.cool.min_on_s);
  EXPECT_FLOAT_EQ(10.f, config.cool.min_off_s);
  EXPECT_FLOAT_EQ(10.f, find_param("min_on_s")->def);
  EXPECT_FLOAT_EQ(10.f, find_param("min_off_s")->def);

  ClimateConfig parsed;
  std::string error;
  ASSERT_TRUE(from_json(R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"}})", &parsed, &error, false)) << error;
  EXPECT_FLOAT_EQ(10.f, parsed.heat.min_on_s) << "a document that does not say gets the default too";
  EXPECT_FLOAT_EQ(10.f, parsed.heat.min_off_s);
}

TEST(ClimateConfigJson, IdIsRequiredOnLoadAndOptionalOnCreate) {
  std::string without_id =
      R"({"name":"Boiler","kind":"pid","sensor_id":"room_temp","heat":{"relay_id":"relay_1"},"mode":"heat"})";
  ClimateConfig parsed;
  std::string error;

  EXPECT_FALSE(from_json(without_id, &parsed, &error, true));
  EXPECT_EQ("id is required", error);
  EXPECT_TRUE(from_json(without_id, &parsed, &error, false)) << error;

  std::string empty_id = R"({"id":"","name":"Boiler","sensor_id":"room_temp","heat":{"relay_id":"relay_1"}})";
  EXPECT_FALSE(from_json(empty_id, &parsed, &error, true));
  EXPECT_EQ("id is required", error);
}

// 7.json saying "id": 7 is not a thermostat with the id "7": as<std::string>() would read one.
TEST(ClimateConfigJson, ALoadedIdMustBeAString) {
  ClimateConfig parsed;
  std::string error;
  for (const char *bad : {"7", "true", R"(["a"])", R"({"a":1})"}) {
    std::string json = std::string(R"({"id":)") + bad +
                       R"(,"name":"B","kind":"pid","sensor_id":"s","heat":{"relay_id":"r"},"mode":"heat"})";
    EXPECT_FALSE(from_json(json, &parsed, &error, true)) << bad;
    EXPECT_EQ("id is required", error) << bad;
  }
}

// A hand-written direction may leave the relay out: the direction is unused, its timing kept.
TEST(ClimateConfigJson, ADirectionWithoutARelayIsUnused) {
  ClimateConfig parsed;
  std::string error;
  ASSERT_TRUE(from_json(R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"cool":{"period_s":60}})", &parsed,
                        &error, false))
      << error;
  EXPECT_FALSE(parsed.supports_cool());
  EXPECT_FLOAT_EQ(60.f, parsed.cool.period_s);
}

// Numbers degrade, structure is refused: a hand edit should cost a clamped gain, not a
// thermostat that vanished.
TEST(ClimateConfigJson, OutOfRangeNumbersAreClamped) {
  ClimateConfig parsed;
  std::string error;
  std::string json =
      R"({"name":"B","kind":"pid","sensor_id":"s","heat":{"relay_id":"r","period_s":999999,"min_on_s":-5},)"
      R"("pid":{"kp":-3,"derivative_samples":9999},"mode":"heat"})";
  ASSERT_TRUE(from_json(json, &parsed, &error, false)) << error;

  EXPECT_FLOAT_EQ(3600.f, parsed.heat.period_s);
  EXPECT_FLOAT_EQ(0.f, parsed.heat.min_on_s);
  EXPECT_FLOAT_EQ(0.f, parsed.pid.kp);
  EXPECT_FLOAT_EQ(100.f, parsed.pid.derivative_samples);
}

TEST(ClimateConfigJson, IntegerParamsAreRounded) {
  ClimateConfig parsed;
  std::string error;
  std::string json =
      R"({"name":"B","kind":"pid","sensor_id":"s","heat":{"relay_id":"r"},"update_interval_s":30.7,"mode":"heat"})";
  ASSERT_TRUE(from_json(json, &parsed, &error, false)) << error;
  EXPECT_FLOAT_EQ(31.f, parsed.update_interval_s);
}

TEST(ClimateConfigJson, StructuralProblemsAreRejectedWithAReason) {
  struct Case {
    const char *json;
    const char *error;
  };
  const Case cases[] = {
      {R"([])", "document is not an object"},
      {R"({"kind":"pid","sensor_id":"s","heat":{"relay_id":"r"}})", "name is required"},
      {R"({"name":"B","kind":"kettle","sensor_id":"s","heat":{"relay_id":"r"}})", "kind must be 'pid' or 'bang_bang'"},
      {R"({"name":"B","kind":"pid","heat":{"relay_id":"r"}})", "sensor_id is required"},
      {R"({"name":"B","kind":"pid","sensor_id":"s"})", "at least one of heat.relay_id / cool.relay_id is required"},
      {R"({"name":"B","kind":"pid","sensor_id":"s","heat":{"relay_id":"r"},"mode":"dry"})",
       "mode must be one of off/heat/cool/heat_cool"},
      {R"({"name":"B","kind":"pid","sensor_id":"s","heat":{"relay_id":"r"},"mode":"cool"})",
       "mode 'cool' needs cool.relay_id"},
      {R"({"name":"B","sensor_id":"s","cool":{"relay_id":"r"},"mode":"heat"})", "mode 'heat' needs heat.relay_id"},
      {R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"mode":"heat_cool"})",
       "mode 'heat_cool' needs both relays"},
      {R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"visual":{"min_temperature":30,"max_temperature":20}})",
       "visual.max_temperature must be above visual.min_temperature"},
      {R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"pid":{"min_integral":1,"max_integral":-1}})",
       "pid.max_integral must not be below pid.min_integral"},
  };

  for (const Case &c : cases) {
    ClimateConfig parsed;
    std::string error;
    EXPECT_FALSE(from_json(c.json, &parsed, &error, false)) << c.json;
    EXPECT_EQ(c.error, error) << c.json;
  }
}

// Both directions would resolve to one claim, and the second request each tick would
// overwrite the first: the thermostat would run silently inert.
TEST(ClimateConfigJson, OneRelayCannotDriveBothDirections) {
  ClimateConfig parsed;
  std::string error;
  std::string json = R"({"name":"B","kind":"pid","sensor_id":"s","mode":"heat_cool",)"
                     R"("heat":{"relay_id":"relay_1"},"cool":{"relay_id":"relay_1"}})";
  EXPECT_FALSE(from_json(json, &parsed, &error, false));
  EXPECT_EQ("heat and cool cannot share one relay", error);
}

// No object id is longer than upstream lets an entity name be, and an uncapped id could grow
// the file past what the next boot loads.
TEST(ClimateConfigJson, AnIdLongerThanAnyObjectIdIsRefused) {
  const std::string longest(ENTITY_ID_MAX_LENGTH, 'a');
  const std::string over = longest + "a";
  auto doc = [](const std::string &sensor, const std::string &heat, const std::string &cool) {
    return R"({"name":"B","sensor_id":")" + sensor + R"(","heat":{"relay_id":")" + heat + R"("},"cool":{"relay_id":")" +
           cool + R"("},"mode":"heat"})";
  };
  struct Case {
    std::string json;
    const char *error;
  };
  const Case cases[] = {
      {doc(over, "r", ""), "sensor_id is longer than 120 characters"},
      {doc(over, "", ""), "sensor_id is longer than 120 characters"},
      {doc("s", over, ""), "heat.relay_id is longer than 120 characters"},
      {doc("s", "r", over), "cool.relay_id is longer than 120 characters"},
      {doc("s", over, over), "heat.relay_id is longer than 120 characters"},
  };
  for (const Case &c : cases) {
    ClimateConfig parsed;
    std::string error;
    EXPECT_FALSE(from_json(c.json, &parsed, &error, false)) << c.json;
    EXPECT_EQ(c.error, error) << c.json;
  }

  ClimateConfig parsed;
  std::string error;
  EXPECT_TRUE(from_json(doc(longest, longest, std::string(ENTITY_ID_MAX_LENGTH, 'b')), &parsed, &error, false))
      << error;

  ClimateConfig built = sample();
  built.cool.relay_id = over;
  EXPECT_FALSE(built.validate(&error));
  EXPECT_EQ("cool.relay_id is longer than 120 characters", error);
}

// Every string at its longest and worst to escape, every number at its widest: the file still
// fits, so a document the rules accept is one the next boot loads.
TEST(ClimateConfigJson, TheLargestDocumentTheRulesAllowFitsTheCap) {
  ClimateConfig c;
  c.version = 65535;
  c.id = std::string(ID_MAX_LENGTH, 'a');
  c.name = std::string(NAME_MAX_LENGTH, '"');
  c.sensor_id = std::string(ENTITY_ID_MAX_LENGTH, '\0');
  c.heat.relay_id = std::string(ENTITY_ID_MAX_LENGTH, '\0');
  c.cool.relay_id = std::string(ENTITY_ID_MAX_LENGTH - 1, '\0') + "x";
  // The widest a float prints.
  const float wide = -1.17549435e-38f;
  c.update_interval_s = c.setpoint = wide;
  c.heat.period_s = c.heat.min_on_s = c.heat.min_off_s = wide;
  c.cool.period_s = c.cool.min_on_s = c.cool.min_off_s = wide;
  c.visual.min_temperature = c.visual.max_temperature = c.visual.step = wide;
  c.safety.sensor_timeout_s = c.safety.max_temperature = wide;
  c.bang_bang.below = c.bang_bang.above = wide;
  PidParams &p = c.pid;
  p.kp = p.ki = p.kd = p.min_integral = p.max_integral = p.starting_integral_term = wide;
  p.output_samples = p.derivative_samples = p.deadband_threshold_low = p.deadband_threshold_high = wide;
  p.deadband_kp_multiplier = p.deadband_ki_multiplier = p.deadband_kd_multiplier = p.deadband_output_samples = wide;
  std::string error;
  ASSERT_TRUE(validate_name(c.name, &error)) << error;

  std::string json;
  ASSERT_EQ(EncodeError::NONE, c.encode(&json));
  EXPECT_NE(std::string::npos, json.find("\\u0000")) << "the worst escape is the one measured";
  EXPECT_LT(json.size(), CONFIG_MAX_BYTES / 2) << json;
}

TEST(ClimateConfigJson, AFileOverTheCapIsNotEncoded) {
  const std::string golden = GOLDEN;
  std::string out = "untouched";
  EXPECT_EQ(EncodeError::TOO_LARGE, sample().encode(&out, golden.size() - 1));
  EXPECT_EQ("untouched", out);
  EXPECT_EQ(EncodeError::NONE, sample().encode(&out, golden.size()));
  EXPECT_EQ(golden, out);
}

// However far the heap gets, the result is the whole document or a refusal, never a cut one.
TEST(ClimateConfigJson, ADocumentAFailedAllocationCutIsNotEncoded) {
  int budget = 0;
  for (;; budget++) {
    ASSERT_LT(budget, 1000) << "the whole document never came out";
    CountdownAllocator allocator(budget);
    std::string out = "untouched";
    const EncodeError result = sample().encode(&out, CONFIG_MAX_BYTES, &allocator);
    if (result == EncodeError::NONE) {
      EXPECT_EQ(GOLDEN, out);
      break;
    }
    EXPECT_EQ(EncodeError::NO_MEMORY, result) << budget;
    EXPECT_EQ("untouched", out) << budget;
  }
  EXPECT_GT(budget, 1) << "the document needs more than one allocation, so some runs were cut";
}

// The id becomes a path component, so a hand-edited file must not send the next save
// somewhere else on the filesystem.
TEST(ClimateConfigJson, ALoadedIdMustBeASlug) {
  ClimateConfig parsed;
  std::string error;
  for (const char *bad : {"../../evil", "Boiler", "with space", "trailing-"}) {
    std::string json = std::string(R"({"id":")") + bad +
                       R"(","name":"B","kind":"pid","sensor_id":"s","heat":{"relay_id":"r"},"mode":"heat"})";
    EXPECT_FALSE(from_json(json, &parsed, &error, true)) << bad;
    EXPECT_EQ("id must be a slug: lowercase letters, digits and single dashes", error) << bad;
  }
}

TEST(ClimateConfigJson, TheDefaultAlgorithmIsBangBang) {
  ClimateConfig parsed;
  std::string error;
  std::string json = R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"mode":"heat"})";
  ASSERT_TRUE(from_json(json, &parsed, &error, false)) << error;
  EXPECT_EQ(ControlKind::BANG_BANG, parsed.kind);
}

// The band is two deviations with a 0.1 floor each, so the switching points cannot collapse.
TEST(ClimateConfigJson, TheBandCannotCollapse) {
  ClimateConfig parsed;
  std::string error;
  std::string json = R"({"name":"B","kind":"bang_bang","sensor_id":"s","heat":{"relay_id":"r"},"mode":"heat",)"
                     R"("setpoint":21,"bang_bang":{"below":0,"above":-5}})";
  ASSERT_TRUE(from_json(json, &parsed, &error, false)) << error;
  EXPECT_FLOAT_EQ(0.1f, parsed.bang_bang.below);
  EXPECT_FLOAT_EQ(0.1f, parsed.bang_bang.above);
  EXPECT_LT(parsed.switch_low(), parsed.switch_high());
}

// The band is not clamped into the visual range: it is a width, and a target on the range's
// edge still needs both points.
TEST(ClimateConfigJson, TheBandIsDerivedFromTheTarget) {
  ClimateConfig parsed;
  std::string error;
  std::string json = R"({"name":"B","kind":"bang_bang","sensor_id":"s","heat":{"relay_id":"r"},"mode":"heat",)"
                     R"("visual":{"min_temperature":5,"max_temperature":45},)"
                     R"("setpoint":45,"bang_bang":{"below":1.5,"above":0.5}})";
  ASSERT_TRUE(from_json(json, &parsed, &error, false)) << error;
  EXPECT_FLOAT_EQ(43.5f, parsed.switch_low());
  EXPECT_FLOAT_EQ(45.5f, parsed.switch_high());
}

TEST(ClimateConfigJson, SetpointsAreHeldInsideTheVisualRange) {
  ClimateConfig parsed;
  std::string error;
  std::string json = R"({"name":"B","kind":"pid","sensor_id":"s","heat":{"relay_id":"r"},"mode":"heat",)"
                     R"("visual":{"min_temperature":10,"max_temperature":30},"setpoint":99})";
  ASSERT_TRUE(from_json(json, &parsed, &error, false)) << error;
  EXPECT_FLOAT_EQ(30.f, parsed.setpoint);

  json = R"({"name":"B","kind":"pid","sensor_id":"s","heat":{"relay_id":"r"},"mode":"heat",)"
         R"("visual":{"min_temperature":10,"max_temperature":30},"setpoint":2})";
  ASSERT_TRUE(from_json(json, &parsed, &error, false)) << error;
  EXPECT_FLOAT_EQ(10.f, parsed.setpoint);
}

// The editor's schema route serves this table; the codec clamps through it.
TEST(ParamTable, ANumberOutsideTheTableIsLeftAndANaNTakesTheDefault) {
  EXPECT_EQ(nullptr, find_param("no_such_param"));
  EXPECT_FLOAT_EQ(123.f, clamp_param("no_such_param", 123.f));
  EXPECT_FLOAT_EQ(30.f, clamp_param("update_interval_s", NAN));
  EXPECT_FLOAT_EQ(1.f, clamp_param("update_interval_s", -5.f));
  EXPECT_FLOAT_EQ(3600.f, clamp_param("update_interval_s", 1e9f));
}

// The document keeps the band whatever `kind` says, so going PID and back does not reset it.
TEST(ClimateConfigJson, SwitchingKindKeepsTheBand) {
  ClimateConfig pid = sample();
  pid.setpoint = 23.f;
  pid.bang_bang.below = 1.5f;
  pid.bang_bang.above = 0.25f;

  ClimateConfig parsed;
  std::string error;
  ASSERT_TRUE(from_json(to_json(pid), &parsed, &error)) << error;
  parsed.kind = ControlKind::BANG_BANG;

  ClimateConfig back;
  ASSERT_TRUE(from_json(to_json(parsed), &back, &error)) << error;
  EXPECT_FLOAT_EQ(23.f, back.setpoint);
  EXPECT_FLOAT_EQ(1.5f, back.bang_bang.below);
  EXPECT_FLOAT_EQ(0.25f, back.bang_bang.above);
}

TEST(ClimateConfigJson, TheNameIsTrimmed) {
  ClimateConfig parsed;
  std::string error;
  ASSERT_TRUE(from_json(R"({"name":"  Boiler \t","sensor_id":"s","heat":{"relay_id":"r"}})", &parsed, &error, false))
      << error;
  EXPECT_EQ("Boiler", parsed.name);
  EXPECT_EQ("Boiler", trim_name("\n\r\f\vBoiler\t "));
}

TEST(NameRules, WhatANameMayBe) {
  std::string error;
  EXPECT_TRUE(validate_name("Living room 2 (south)", &error)) << error;
  EXPECT_TRUE(validate_name(std::string(48, 'a'), &error)) << error;

  struct Case {
    std::string name;
    const char *error;
  };
  const Case cases[] = {
      {"", "Name is required"},
      {std::string(49, 'a'), "Name is longer than 48 characters"},
      {"Up/down", "Name cannot contain '/'"},
      {"Back\\slash", "Name cannot contain '\\'"},
      {"Кухня", "Use printable ASCII characters only"},
      {"Tab\there", "Use printable ASCII characters only"},
      // The whole name is checked for each rule in turn, not character by character.
      {"a/b\tc", "Use printable ASCII characters only"},
      {"a\\b/c", "Name cannot contain '/'"},
  };
  for (const Case &c : cases) {
    EXPECT_FALSE(validate_name(c.name, &error)) << c.name;
    EXPECT_EQ(c.error, error) << c.name;
  }
}

TEST(NameRules, TheDocumentRefusesABrokenName) {
  ClimateConfig parsed;
  std::string error;
  EXPECT_FALSE(from_json(R"({"name":"a/b","sensor_id":"s","heat":{"relay_id":"r"}})", &parsed, &error, false));
  EXPECT_EQ("Name cannot contain '/'", error);
  EXPECT_FALSE(from_json(R"({"name":"   ","sensor_id":"s","heat":{"relay_id":"r"}})", &parsed, &error, false));
  EXPECT_EQ("Name is required", error);
}

// The editor reports the first rule a document breaks, in the order its client mirrors: the
// structure first, the mode word between the ranges and the mode's relays, the name rules last.
TEST(ClimateConfigJson, TheFirstBrokenRuleIsTheOneReported) {
  struct Case {
    const char *json;
    const char *error;
  };
  const Case cases[] = {
      {R"({"name":"a/b","heat":{"relay_id":"r"}})", "sensor_id is required"},
      {R"({"name":"a/b","sensor_id":"s","heat":{"relay_id":"r"},"mode":"dry"})",
       "mode must be one of off/heat/cool/heat_cool"},
      {R"({"name":"B","heat":{"relay_id":"r"},"mode":"dry"})", "sensor_id is required"},
      {R"({"name":"B","sensor_id":"s","heat":{"relay_id":"r"},"mode":"dry","pid":{"min_integral":1,"max_integral":-1}})",
       "pid.max_integral must not be below pid.min_integral"},
      {R"({"name":"a/b","sensor_id":"s","heat":{"relay_id":"r"},"mode":"cool"})", "mode 'cool' needs cool.relay_id"},
      {R"({"name":"a/b","kind":"kettle"})", "kind must be 'pid' or 'bang_bang'"},
  };
  for (const Case &c : cases) {
    ClimateConfig parsed;
    std::string error;
    EXPECT_FALSE(from_json(c.json, &parsed, &error, false)) << c.json;
    EXPECT_EQ(c.error, error) << c.json;
  }
}

// A number where a string belongs is missing, not its JSON text.
TEST(ClimateConfigJson, ANumberIsNoName) {
  ClimateConfig parsed;
  std::string error;
  EXPECT_FALSE(from_json(R"({"name":5,"sensor_id":"s","heat":{"relay_id":"r"}})", &parsed, &error, false));
  EXPECT_EQ("name is required", error);
  EXPECT_FALSE(from_json(R"({"name":"B","sensor_id":5,"heat":{"relay_id":"r"}})", &parsed, &error, false));
  EXPECT_EQ("sensor_id is required", error);
  EXPECT_FALSE(from_json(R"({"name":"B","sensor_id":"s","heat":{"relay_id":1}})", &parsed, &error, false));
  EXPECT_EQ("at least one of heat.relay_id / cool.relay_id is required", error);
  ASSERT_TRUE(from_json(R"({"id":7,"name":"B","sensor_id":"s","heat":{"relay_id":"r"}})", &parsed, &error, false));
  EXPECT_EQ("", parsed.id);
}

// What Home Assistant and every object-id-keyed record know the entity by.
TEST(NameRules, TheObjectIdIsWhatESPHomeDerives) {
  EXPECT_EQ("living_room", object_id_of_name("Living Room"));
  EXPECT_EQ("room_1", object_id_of_name("Room 1"));
  EXPECT_EQ("room_1", object_id_of_name("Room_1"));
  EXPECT_EQ("room_1", object_id_of_name("Room.1"));
  EXPECT_EQ(fnv1_hash_object_id("Room 1", 6), fnv1_hash(object_id_of_name("Room 1")));
}

TEST(NameRules, TheKeyIsTheNameAsAPersonReadsIt) {
  EXPECT_EQ("living room", name_key("  Living   Room "));
  EXPECT_EQ(name_key("LIVING ROOM"), name_key("living room"));
  EXPECT_NE(name_key("Room 1"), name_key("Room_1"));
  // A YAML climate's name may be anything; its non-ASCII bytes are compared as they are.
  EXPECT_EQ("\xd0\x9a\xd1\x83\xd1\x85\xd0\xbd\xd1\x8f 1", name_key("\xd0\x9a\xd1\x83\xd1\x85\xd0\xbd\xd1\x8f  1"));
}

TEST(SlugifyId, ReducesToASafeStem) {
  EXPECT_EQ("living-room", slugify_id("Living Room"));
  EXPECT_EQ("living-room", slugify_id("  living   room  "));
  EXPECT_EQ("boiler-2", slugify_id("Boiler #2"));
  EXPECT_EQ("teplyy-pol", slugify_id("teplyy_pol"));
  EXPECT_EQ("climate", slugify_id("!!!")) << "nothing survived, so the fallback stands";
  EXPECT_EQ("climate", slugify_id(""));
  EXPECT_EQ(48u, slugify_id(std::string(80, 'a')).size());
  EXPECT_EQ(std::string(47, 'a'), slugify_id(std::string(47, 'a') + " b")) << "a cut never ends on a dash";
  EXPECT_EQ("2", slugify_id("\xd0\x9a\xd1\x83\xd1\x85\xd0\xbd\xd1\x8f 2")) << "only ASCII letters survive";
}

// A suffixed id still fits the limit and still passes the slug check on the next boot.
TEST(SlugifyId, ASuffixNeverOverrunsTheLimit) {
  const std::string base = slugify_id(std::string(50, 'a'));
  ASSERT_EQ(48u, base.size());
  const std::string suffixed = id_with_suffix(base, 12);
  EXPECT_EQ(48u, suffixed.size());
  EXPECT_EQ(suffixed, slugify_id(suffixed));
  EXPECT_EQ("-12", suffixed.substr(45));

  // A cut that would end on a dash drops it rather than leave "--".
  const std::string dashed = std::string(44, 'a') + "-bbb";
  ASSERT_EQ(dashed, slugify_id(dashed));
  EXPECT_EQ(std::string(44, 'a') + "-12", id_with_suffix(dashed, 12));
}

// A running thermostat holds a ClimateConfig *; a vector<ClimateConfig> would move them all on
// the next insert.
TEST(ConfigStoreTest, PointersSurviveLaterInserts) {
  ConfigStore store;
  ClimateConfig first = sample();
  first.id = "first";
  ClimateConfig *held = store.add(first);

  for (int i = 0; i < 32; i++) {
    ClimateConfig extra = sample();
    extra.id = "extra-" + std::to_string(i);
    store.add(extra);
  }
  EXPECT_EQ("first", held->id);
  EXPECT_EQ(held, store.get("first"));

  EXPECT_TRUE(store.remove("extra-0"));
  store.sort_by_id();
  EXPECT_EQ("first", held->id) << "and neither an erase nor a sort moves it";
  EXPECT_FALSE(store.remove("extra-0")) << "gone already";
}

TEST(ConfigStoreTest, SortsById) {
  ConfigStore store;
  for (const char *id : {"zulu", "alpha", "mike"}) {
    ClimateConfig c = sample();
    c.id = id;
    store.add(c);
  }
  store.sort_by_id();
  EXPECT_EQ("alpha", store.all()[0]->id);
  EXPECT_EQ("mike", store.all()[1]->id);
  EXPECT_EQ("zulu", store.all()[2]->id);
}

}  // namespace esphome::climate_hub::testing
