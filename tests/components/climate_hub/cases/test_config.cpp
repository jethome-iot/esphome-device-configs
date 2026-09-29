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
      {"", "name is required"},
      {std::string(49, 'a'), "name must be at most 48 characters"},
      {"Up/down", "name must not contain '/' or '\\'"},
      {"Back\\slash", "name must not contain '/' or '\\'"},
      {"Кухня", "name must use printable ASCII characters only"},
      {"Tab\there", "name must use printable ASCII characters only"},
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
  EXPECT_EQ("name must not contain '/' or '\\'", error);
  EXPECT_FALSE(from_json(R"({"name":"   ","sensor_id":"s","heat":{"relay_id":"r"}})", &parsed, &error, false));
  EXPECT_EQ("name is required", error);
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
}

TEST(SlugifyId, ReducesToASafeStem) {
  EXPECT_EQ("living-room", slugify_id("Living Room"));
  EXPECT_EQ("living-room", slugify_id("  living   room  "));
  EXPECT_EQ("boiler-2", slugify_id("Boiler #2"));
  EXPECT_EQ("teplyy-pol", slugify_id("teplyy_pol"));
  EXPECT_EQ("climate", slugify_id("!!!")) << "nothing survived, so the fallback stands";
  EXPECT_EQ("climate", slugify_id(""));
  EXPECT_EQ(48u, slugify_id(std::string(80, 'a')).size());
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

// "Living Room" and "living room" slugify alike; keying the file on the id rather than the
// name is what stops the second one silently overwriting the first.
TEST(ConfigStoreTest, CollidingNamesGetDistinctIds) {
  ConfigStore store;
  ClimateConfig a = sample();
  a.id = store.unique_id_from(slugify_id("Living Room"));
  store.add(a);

  ClimateConfig b = sample();
  b.id = store.unique_id_from(slugify_id("living room"));
  store.add(b);

  EXPECT_EQ("living-room", a.id);
  EXPECT_EQ("living-room-2", b.id);
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

  store.remove("extra-0");
  store.sort_by_id();
  EXPECT_EQ("first", held->id) << "and neither an erase nor a sort moves it";
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
