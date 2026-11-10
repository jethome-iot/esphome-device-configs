// Saved gain sets: the lines of a sets file and what reads and rewrites them, then a thermostat's
// sets through its life: saved by hand and by a calibration, renamed, deleted, applied, derived for
// another rule, and what a removal, a create and a restore do with the file.
#include <cstring>
#include "common.h"
#include "esphome/components/climate_hub/gain_sets.h"
#include "esphome/components/climate_hub/param_table.h"
#include "room.h"

namespace esphome::climate_hub::testing {
namespace {

// 2026-11-06 14:05 UTC, which the cases' clock gives as local time.
constexpr time_t SAVED_AT = 1793973900;

// A set of the kind a calibration saves.
GainSet calibrated(uint32_t n, const char *name) {
  GainSet set;
  set.n = n;
  set.name = name;
  set.direction = Direction::HEAT;
  set.source = GAIN_SET_CALIBRATION;
  set.rule = "zn_pi";
  set.ku = 12.5f;
  set.pu = 3600.f;
  set.gains = PidGains{5.625f, 0.0019f, 0.f};
  set.flags = {"uneven"};
  set.time = static_cast<int64_t>(SAVED_AT);
  return set;
}

std::string line_of(const GainSet &set) {
  std::string line;
  EXPECT_TRUE(encode_gain_set(set, &line));
  return line;
}

// A calibration's result as a file holds it: Ku 10 and Pu 1200 give round gains for every rule.
const char *const MEASURED =
    R"({"n":1,"name":"Measured","direction":"heat","source":"calibration","rule":"zn_pi","ku":10,"pu":1200,)"
    R"("kp":4.5,"ki":0.0045,"kd":0,"flags":["uneven","clamped"],"time":null,"from":null})";

class GainSets : public HubTest {
 protected:
  // A running PID thermostat on Room and Relay 1, heating to 21, with a first reading.
  std::string start(ClimateConfig config = draft("Living room"), float reading = 18.f) {
    config.setpoint = 21.f;
    const std::string id = this->create(config).id;
    hub().ms += 1000;
    entities().room.publish_state(reading);
    hub().loop();
    return id;
  }

  // Heat on Relay 1 and cool on Relay 2, each with gains of its own, in mode cool.
  static ClimateConfig studio() {
    ClimateConfig c = draft("Studio");
    c.cool.relay_id = "relay_2";
    c.mode = HubMode::COOL;
    c.pid.heat = PidGains{1.5f, 0.25f, 2.f};
    c.pid.cool = PidGains{0.75f, 0.5f, 0.f};
    return c;
  }

  // Every set a listing gives, as the web server's task reads them.
  static std::vector<GainSet> sets(const std::string &id) {
    std::vector<GainSet> out;
    Result result;
    std::unique_ptr<GainSetReader> reader = hub().open_gain_sets(id, &result);
    EXPECT_TRUE(result.ok) << result.error;
    GainSet set;
    while (reader != nullptr && reader->next(&set))
      out.push_back(set);
    return out;
  }

  static std::vector<uint32_t> numbers(const std::string &id) {
    std::vector<uint32_t> out;
    for (const GainSet &set : sets(id))
      out.push_back(set.n);
    return out;
  }

  // The sets file as a restore or a hand edit leaves it.
  void put_sets(const std::string &id, const std::string &text) {
    mkdir(this->folder().c_str(), 0755);
    mkdir(this->gains().c_str(), 0755);
    write_file(this->sets_of(id), text);
  }

  static GainSet add(const std::string &id, optional<Direction> direction = nullopt, const std::string &name = "") {
    GainSet set;
    const Result result = hub().add_gain_set(id, direction, name, &set);
    EXPECT_TRUE(result.ok) << result.error;
    return set;
  }

  // Swings `id`'s room in a room with radiators until its calibration ends.
  static void calibrate(const std::string &id) {
    const RoomModel model = radiator_room();
    Room room(model);
    entities().room.publish_state(room.reading());
    ASSERT_TRUE(hub().start_autotune(id, nullopt, AutotuneRule::ZN_PI).ok);
    const auto step_ms = static_cast<uint32_t>(model.step_s * 1000.f);
    for (uint32_t i = 0; i < 24 * 3600 / 10; i++) {
      hub().ms += step_ms;
      hub().loop();
      entities().room.publish_state(room.step(entities().relay1.state));
      const AutotuneRun *run = hub().autotune(id);
      if (run != nullptr && !run->running())
        return;
    }
  }

  static size_t lines_in(const std::string &text) { return std::count(text.begin(), text.end(), '\n'); }
};

}  // namespace

// --- The line ---

TEST(GainSetCodec, ALineHoldsItsKeysInAFixedOrder) {
  EXPECT_EQ(R"({"n":12,"name":"2026-10-26 14:05 ZN PI","direction":"heat","source":"calibration","rule":"zn_pi",)"
            R"("ku":12.5,"pu":3600,"kp":5.625,"ki":0.0019,"kd":0,"flags":["uneven"],"time":1793973900,"from":null})",
            line_of(calibrated(12, "2026-10-26 14:05 ZN PI")));

  GainSet manual;
  manual.n = 3;
  manual.name = "Зима";
  manual.direction = Direction::COOL;
  manual.gains = PidGains{0.6f, 0.0025f, 0.f};
  manual.from = 2u;
  EXPECT_EQ(R"({"n":3,"name":"Зима","direction":"cool","source":"manual","rule":null,"ku":null,"pu":null,)"
            R"("kp":0.6,"ki":0.0025,"kd":0,"flags":[],"time":null,"from":2})",
            line_of(manual));
}

// No set the device makes comes near it: a name is 48 characters at most.
TEST(GainSetCodec, ALineOverTheCapIsNotWritten) {
  GainSet set = calibrated(1, "x");
  set.name = std::string(GAIN_SET_LINE_MAX_BYTES, 'x');
  std::string line = "untouched";
  EXPECT_FALSE(encode_gain_set(set, &line));
  EXPECT_EQ("untouched", line);
  EXPECT_FALSE(append_gain_set(".storage/never-written.jsonl", set));
  EXPECT_FALSE(file_exists(".storage/never-written.jsonl"));
}

TEST(GainSetCodec, ALineReadsBackAsTheSetItWas) {
  GainSet back;
  ASSERT_TRUE(decode_gain_set(line_of(calibrated(7, "Winter")), &back));
  EXPECT_EQ(7u, back.n);
  EXPECT_EQ("Winter", back.name);
  EXPECT_EQ(Direction::HEAT, back.direction);
  EXPECT_EQ(GAIN_SET_CALIBRATION, back.source);
  EXPECT_EQ("zn_pi", back.rule);
  EXPECT_EQ(12.5f, back.ku);
  EXPECT_EQ(3600.f, back.pu);
  EXPECT_EQ(5.625f, back.gains.kp);
  EXPECT_EQ(as_stored(0.0019f), back.gains.ki);
  EXPECT_EQ(std::vector<std::string>{"uneven"}, back.flags);
  EXPECT_TRUE(back.has_flag("uneven"));
  EXPECT_FALSE(back.has_flag("clamped"));
  ASSERT_TRUE(back.time.has_value());
  EXPECT_EQ(SAVED_AT, *back.time);
  EXPECT_FALSE(back.from.has_value());
  EXPECT_TRUE(back.has_measurement());

  // What a newer firmware may write besides is no reason to drop the set.
  ASSERT_TRUE(
      decode_gain_set(R"({"n":2,"name":"x","direction":"cool","source":"imported","rule":"zn_q","kp":1,"ki":2,"kd":3,)"
                      R"("flags":["drifting",5],"note":{"by":"v9"},"ku":"high","time":"today","from":-1})",
                      &back));
  EXPECT_EQ("imported", back.source);
  EXPECT_EQ("zn_q", back.rule);
  EXPECT_EQ(std::vector<std::string>{"drifting"}, back.flags);
  EXPECT_FALSE(back.has_measurement());
  EXPECT_FALSE(back.time.has_value());
  EXPECT_FALSE(back.from.has_value());
}

// A line the device would only stumble over is no set: it is not listed, found or applied.
TEST(GainSetCodec, ALineNoSetCanBeAppliedFromIsNoSet) {
  const char *const good = R"("name":"x","direction":"heat","kp":1,"ki":0,"kd":0)";
  for (const std::string &line :
       {std::string(""), std::string("not json"), std::string("[1,2]"), std::string(R"({"n":3})"),
        std::string("{") + good + "}", std::string(R"({"n":0,)") + good + "}", std::string(R"({"n":-1,)") + good + "}",
        std::string(R"({"n":1.5,)") + good + "}", std::string(R"({"n":"1",)") + good + "}",
        std::string(R"({"n":1,"name":5,"direction":"heat","kp":1,"ki":0,"kd":0})"),
        std::string(R"({"n":1,"direction":"heat","kp":1,"ki":0,"kd":0})"),
        std::string(R"({"n":1,"name":"x","direction":"both","kp":1,"ki":0,"kd":0})"),
        std::string(R"({"n":1,"name":"x","direction":"heat","ki":0,"kd":0})"),
        std::string(R"({"n":1,"name":"x","direction":"heat","kp":"1","ki":0,"kd":0})"),
        std::string(R"({"n":1,"name":"x","direction":"heat","kp":null,"ki":0,"kd":0})"),
        std::string(R"({"n":1,"name":"x","direction":"heat","kp":1,"ki":0,"kd":0,"deep":[[[1]]]})"),
        std::string(R"({"n":1,"name":"x","direction":"heat","kp":1,"ki":0,"kd":0)")}) {
    GainSet set;
    EXPECT_FALSE(decode_gain_set(line, &set)) << line;
  }
}

TEST(GainSetCodec, ANameIsUpTo48CharactersOfAnyScript) {
  std::string error;
  std::string cyrillic;
  std::string clef;
  for (int i = 0; i < 48; i++) {
    cyrillic += "я";
    clef += "𝄞";
  }
  for (const std::string &name : {std::string("Winter"), std::string(48, 'a'), cyrillic, clef,
                                  std::string("Зима 🌨 2026 / \"floor\" \\ eco")}) {
    EXPECT_TRUE(validate_set_name(name, &error)) << name << ": " << error;
  }
  struct Refusal {
    std::string name;
    const char *error;
  };
  for (const Refusal &refusal : {
           Refusal{"", "Name is required"},
           Refusal{std::string(49, 'a'), "Name is longer than 48 characters"},
           Refusal{cyrillic + "я", "Name is longer than 48 characters"},
           Refusal{clef + "a", "Name is longer than 48 characters"},
           Refusal{"\xC0\x80", "Name is not valid UTF-8"},
           Refusal{"\xED\xA0\x80", "Name is not valid UTF-8"},
           Refusal{"\xF4\x90\x80\x80", "Name is not valid UTF-8"},
           Refusal{"\xE0\x80\xAF", "Name is not valid UTF-8"},
           Refusal{"\xF0\x80\x80\xAF", "Name is not valid UTF-8"},
           Refusal{"ab\xE2\x82", "Name is not valid UTF-8"},
           Refusal{"\x80", "Name is not valid UTF-8"},
           Refusal{"\xFF", "Name is not valid UTF-8"},
           Refusal{"a\tb", "Name cannot contain control characters"},
           Refusal{"a\x7F", "Name cannot contain control characters"},
           Refusal{"a\xC2\x85", "Name cannot contain control characters"},
           Refusal{std::string("a\0b", 3), "Name cannot contain control characters"},
       }) {
    error.clear();
    EXPECT_FALSE(validate_set_name(refusal.name, &error)) << refusal.name;
    EXPECT_EQ(refusal.error, error) << refusal.name;
  }
  EXPECT_FALSE(validate_set_name("", nullptr));
}

TEST(GainSetCodec, ADefaultNameIsTheLocalTimeAndTheRule) {
  const ESPTime now = ESPTime::from_epoch_utc(SAVED_AT);
  ASSERT_TRUE(now.is_valid());
  EXPECT_EQ("2026-11-06 14:05 ZN PI", default_set_name(now, Direction::HEAT, rule_label(AutotuneRule::ZN_PI), 4));
  EXPECT_EQ("2026-11-06 14:05 manual", default_set_name(now, Direction::COOL, GAIN_SET_MANUAL, 4));
  EXPECT_EQ("Heat ZN PI #3", default_set_name(ESPTime{}, Direction::HEAT, rule_label(AutotuneRule::ZN_PI), 3));
  EXPECT_EQ("Cool manual #12", default_set_name(ESPTime{}, Direction::COOL, GAIN_SET_MANUAL, 12));
  EXPECT_STREQ("ZN PID", rule_label(AutotuneRule::ZN_PID));
  EXPECT_STREQ("Pessen", rule_label(AutotuneRule::PESSEN));
  EXPECT_STREQ("Some overshoot", rule_label(AutotuneRule::SOME_OVERSHOOT));
  EXPECT_STREQ("No overshoot", rule_label(AutotuneRule::NO_OVERSHOOT));
  EXPECT_STREQ("ZN PI", rule_label(static_cast<AutotuneRule>(99)));
  std::string error;
  for (AutotuneRule rule : {AutotuneRule::ZN_PI, AutotuneRule::ZN_PID, AutotuneRule::PESSEN,
                            AutotuneRule::SOME_OVERSHOOT, AutotuneRule::NO_OVERSHOOT}) {
    EXPECT_TRUE(validate_set_name(default_set_name(now, Direction::COOL, rule_label(rule), 1), &error));
    EXPECT_TRUE(validate_set_name(default_set_name(ESPTime{}, Direction::COOL, rule_label(rule), 4294967295u), &error));
  }
}

TEST_F(HubTest, ALineReaderCutsALongLineAndHandsOverTheRest) {
  const std::string path = this->base_ + "/lines";
  const std::string long_line(3000, 'x');
  write_file(path, "short\n" + long_line + "\n\nlast without newline");
  FILE *file = fopen(path.c_str(), "r");
  ASSERT_NE(nullptr, file);
  LineReader lines(file, 1024);
  std::string line;
  bool whole = false;
  ASSERT_TRUE(lines.next(&line, &whole));
  EXPECT_EQ("short", line);
  EXPECT_TRUE(whole);
  ASSERT_TRUE(lines.next(&line, &whole));
  EXPECT_FALSE(whole);
  EXPECT_EQ(std::string(1024, 'x'), line);
  std::string rest;
  EXPECT_TRUE(lines.rest([&rest](const char *data, size_t len) {
    rest.append(data, len);
    return true;
  }));
  EXPECT_EQ(std::string(3000 - 1024, 'x'), rest);
  EXPECT_TRUE(lines.rest(nullptr)) << "nothing more of it";
  ASSERT_TRUE(lines.next(&line, &whole));
  EXPECT_EQ("", line);
  ASSERT_TRUE(lines.next(&line, &whole));
  EXPECT_EQ("last without newline", line);
  EXPECT_TRUE(whole);
  EXPECT_FALSE(lines.next(&line, &whole));
  EXPECT_FALSE(lines.failed());
  fclose(file);

  // A line exactly as long as the cap is whole, and the rest of an unread cut line is skipped.
  write_file(path, std::string(8, 'a') + "\n" + std::string(20, 'b') + "\nc");
  file = fopen(path.c_str(), "r");
  LineReader small(file, 8);
  ASSERT_TRUE(small.next(&line, &whole));
  EXPECT_TRUE(whole);
  ASSERT_TRUE(small.next(&line, &whole));
  EXPECT_FALSE(whole);
  ASSERT_TRUE(small.next(&line, &whole));
  EXPECT_EQ("c", line);
  fclose(file);
  ::remove(path.c_str());

  LineReader none(nullptr, 8);
  EXPECT_FALSE(none.next(&line, &whole));
}

// --- The file ---

// A hand edit, a newer firmware or a power cut may leave lines that are no set: a rewrite keeps
// every one of them byte for byte, and the rest of a renamed set's line too.
TEST_F(GainSets, ARewriteKeepsEveryOtherLineAsItWas) {
  const std::string id = this->start();
  const std::string newer =
      R"({"n":2,"name":"Spring","direction":"heat","source":"manual","kp":1,"ki":0.001,"kd":0,"by":{"firmware":9}})";
  const std::string long_line = R"({"n":3,"note":")" + std::string(3000, 'z') + R"("})";
  const std::string text = line_of(calibrated(1, "Winter")) + "\n" + "not json at all\n" + newer + "\n" + long_line +
                           "\n" + "\n" + R"({"n":4,"name":"Odd","direction":"sideways","kp":1,"ki":0,"kd":0})" + "\n" +
                           line_of(calibrated(5, "Last")) + "\n";
  this->put_sets(id, text);
  EXPECT_EQ((std::vector<uint32_t>{1, 2, 5}), numbers(id));

  GainSet renamed;
  ASSERT_TRUE(hub().rename_gain_set(id, 2, "  Весна  ", &renamed).ok);
  EXPECT_EQ("Весна", renamed.name);
  EXPECT_EQ(1.f, renamed.gains.kp);
  const std::string after_rename = read_file(this->sets_of(id));
  std::string want = text;
  want.replace(want.find(newer), newer.size(),
               R"({"n":2,"name":"Весна","direction":"heat","source":"manual","kp":1,"ki":0.001,"kd":0,)"
               R"("by":{"firmware":9}})");
  // The blank line goes: it carries nothing.
  want.replace(want.find("\n\n"), 2, "\n");
  EXPECT_EQ(want, after_rename);

  ASSERT_TRUE(hub().delete_gain_set(id, 1).ok);
  want.erase(0, line_of(calibrated(1, "Winter")).size() + 1);
  EXPECT_EQ(want, read_file(this->sets_of(id)));
  EXPECT_EQ((std::vector<uint32_t>{2, 5}), numbers(id));
  EXPECT_FALSE(file_exists(this->sets_of(id) + ".tmp"));
}

TEST_F(GainSets, ADeleteHandsBackTheSetItRemoved) {
  const std::string id = this->start();
  this->put_sets(id, line_of(calibrated(1, "Winter")) + "\n" + line_of(calibrated(2, "Spring")) + "\n");
  GainSet removed;
  EXPECT_EQ(GainSetRewrite::DONE, rewrite_gain_sets(this->sets_of(id), 1, nullptr, &removed));
  EXPECT_EQ("Winter", removed.name);
  EXPECT_EQ(GainSetRewrite::NOT_FOUND, rewrite_gain_sets(this->sets_of(id), 1, nullptr, &removed));
  EXPECT_EQ(std::vector<uint32_t>{2}, numbers(id));
}

// A power cut in the middle of an append leaves half a line at the end: it is no set, and the
// next one goes on a line of its own.
TEST_F(GainSets, ATornLastLineStaysALineOfItsOwn) {
  const std::string id = this->start();
  const std::string torn = R"({"n":2,"name":"Ha)";
  this->put_sets(id, line_of(calibrated(1, "Winter")) + "\n" + torn);
  EXPECT_EQ(std::vector<uint32_t>{1}, numbers(id));
  const GainSet added = add(id, nullopt, "After the cut");
  EXPECT_EQ(2u, added.n);
  EXPECT_EQ(line_of(calibrated(1, "Winter")) + "\n" + torn + "\n" + line_of(added) + "\n",
            read_file(this->sets_of(id)));
  EXPECT_EQ((std::vector<uint32_t>{1, 2}), numbers(id));
  ASSERT_TRUE(hub().rename_gain_set(id, 2, "Kept").ok);
  EXPECT_NE(std::string::npos, read_file(this->sets_of(id)).find(torn + "\n")) << "kept by a rewrite";
}

TEST_F(GainSets, ANumberIsNeverGivenTwice) {
  const std::string id = this->start();
  for (uint32_t n = 1; n <= 3; n++)
    EXPECT_EQ(n, add(id).n);
  ASSERT_TRUE(hub().delete_gain_set(id, 3).ok);
  EXPECT_EQ((std::vector<uint32_t>{1, 2}), numbers(id));
  EXPECT_EQ(4u, add(id).n) << "the deleted 3 is kept as a number";
  ASSERT_TRUE(hub().delete_gain_set(id, 4).ok);
  ASSERT_TRUE(hub().delete_gain_set(id, 2).ok);
  ASSERT_TRUE(hub().rename_gain_set(id, 1, "Only").ok);
  const std::string text = read_file(this->sets_of(id));
  EXPECT_EQ(2u, lines_in(text)) << "one line keeps the highest number, however many went: " << text;
  EXPECT_NE(std::string::npos, text.find("{\"n\":4}\n"));
  ASSERT_TRUE(hub().delete_gain_set(id, 1).ok);
  EXPECT_TRUE(numbers(id).empty());
  EXPECT_EQ("{\"n\":4}\n", read_file(this->sets_of(id)));
  EXPECT_EQ(5u, add(id).n);
  EXPECT_EQ(404, hub().delete_gain_set(id, 4).code) << "a kept number is no set";

  // A line no set could be read from still holds its number.
  this->put_sets(id, R"({"n":9,"name":"x","direction":"both","kp":1,"ki":0,"kd":0})"
                     "\n");
  EXPECT_EQ(10u, add(id).n);
}

// The web server's task reads the file while the loop task runs: an add, a calibration's among
// them, waits in RAM until the reader is gone, and a rewrite is refused meanwhile.
TEST_F(GainSets, AnAddWaitsWhileAReaderHasTheFileOpen) {
  hub().clock = ESPTime::from_epoch_utc(SAVED_AT);
  const std::string id = this->start();
  add(id, nullopt, "First");
  Result result;
  std::unique_ptr<GainSetReader> reader = hub().open_gain_sets(id, &result);
  ASSERT_NE(nullptr, reader);
  EXPECT_EQ(1u, hub().open_readers());
  const std::string before = read_file(this->sets_of(id));

  const GainSet waiting = add(id, nullopt, "Second");
  EXPECT_EQ(2u, waiting.n);
  calibrate(id);
  const AutotuneRun *run = hub().autotune(id);
  ASSERT_EQ(AutotuneState::SUCCEEDED, run->state());
  EXPECT_EQ(3u, run->saved_n()) << "numbered at once";
  EXPECT_EQ(2u, hub().pending_sets());
  EXPECT_EQ(before, read_file(this->sets_of(id)));
  GainSet set;
  ASSERT_TRUE(reader->next(&set));
  EXPECT_EQ("First", set.name);
  EXPECT_FALSE(reader->next(&set)) << "the reader sees the file as it was";

  EXPECT_EQ(503, hub().rename_gain_set(id, 1, "Renamed").code);
  EXPECT_EQ("The gain sets are being read; try again", hub().delete_gain_set(id, 1).error);
  // Applying reads, which the reader allows, and finds a set that waits.
  EXPECT_TRUE(hub().apply_gain_set(id, 2).ok);

  hub().loop();
  EXPECT_EQ(2u, hub().pending_sets()) << "not while the reader is open";
  reader.reset();
  EXPECT_EQ(0u, hub().open_readers());
  hub().loop();
  EXPECT_EQ(0u, hub().pending_sets());
  EXPECT_EQ((std::vector<uint32_t>{1, 2, 3}), numbers(id));
  EXPECT_EQ("2026-11-06 14:05 ZN PI", sets(id)[2].name);
  EXPECT_TRUE(hub().rename_gain_set(id, 1, "Renamed").ok);
}

TEST_F(GainSets, WhatWaitsIsWrittenBeforeTheNextReaderAndAtShutdown) {
  const std::string id = this->start();
  Result result;
  auto reader = hub().open_gain_sets(id, &result);
  add(id);
  reader.reset();
  // No loop pass in between: the next listing still sees it.
  EXPECT_EQ(std::vector<uint32_t>{1}, numbers(id));

  reader = hub().open_gain_sets(id, &result);
  add(id);
  hub().on_shutdown();
  EXPECT_EQ(0u, hub().pending_sets());
  EXPECT_EQ(2u, lines_in(read_file(this->sets_of(id))));
}

TEST_F(GainSets, ARemovedThermostatTakesItsSetsWithIt) {
  const std::string id = this->start();
  add(id);
  ASSERT_TRUE(file_exists(this->sets_of(id)));
  ASSERT_TRUE(hub().remove(id).ok);
  EXPECT_FALSE(file_exists(this->sets_of(id)));

  // A partition that keeps the file: the next thermostat under the id starts without its sets.
  const std::string again = this->start();
  ASSERT_EQ(id, again);
  add(again);
  hub().undeletable.push_back(".jsonl");
  LogCapture::instance().clear();
  ASSERT_TRUE(hub().remove(again).ok);
  EXPECT_TRUE(LogCapture::instance().has("Cannot delete '" + this->sets_of(id) + "'"));
  EXPECT_TRUE(file_exists(this->sets_of(id)));
  hub().undeletable.clear();
  const std::string third = this->start();
  EXPECT_FALSE(file_exists(this->sets_of(third)));
  EXPECT_TRUE(sets(third).empty());
}

TEST_F(GainSets, ARemoveWhileAReaderIsOpenDeletesOnceItCloses) {
  const std::string id = this->start();
  add(id);
  Result result;
  auto reader = hub().open_gain_sets(id, &result);
  ASSERT_TRUE(hub().remove(id).ok);
  EXPECT_TRUE(file_exists(this->sets_of(id))) << "open on the other task";
  reader.reset();
  hub().loop();
  EXPECT_FALSE(file_exists(this->sets_of(id)));
}

// A thermostat deleted under an id may have left its sets behind, the partition keeping them, or
// a restore that brought them was refused: a new thermostat under that id does not inherit them.
TEST_F(GainSets, ACreateClearsWhatWasLeftUnderItsId) {
  this->put_sets("living-room", line_of(calibrated(1, "Old")) + "\n");
  const std::string id = this->start();
  ASSERT_EQ("living-room", id);
  EXPECT_FALSE(file_exists(this->sets_of(id)));
  EXPECT_TRUE(sets(id).empty());
  EXPECT_EQ(1u, add(id).n);
}

// A restore uploads a backup's sets file as it is, then brings the thermostat back: the sets
// stay, for a thermostat it creates and for one it replaces.
TEST_F(GainSets, ARestoreKeepsTheSets) {
  const std::string text = line_of(calibrated(1, "Winter")) + "\n" + line_of(calibrated(4, "Spring")) + "\n";
  this->put_sets("lounge", text);
  ClimateConfig doc = draft("Lounge");
  doc.id = "lounge";
  ASSERT_TRUE(hub().restore(doc).ok);
  EXPECT_EQ(text, read_file(this->sets_of("lounge")));
  EXPECT_EQ((std::vector<uint32_t>{1, 4}), numbers("lounge"));
  ASSERT_TRUE(hub().restore(doc).ok);
  EXPECT_EQ(text, read_file(this->sets_of("lounge")));
  EXPECT_EQ(5u, add("lounge").n);
  this->reboot();
  EXPECT_EQ((std::vector<uint32_t>{1, 4, 5}), numbers("lounge"));
}

// The boot leaves a file it refuses exactly as it is, and the sets beside it too.
TEST_F(GainSets, ARefusedThermostatLeavesItsSetsAlone) {
  write_file(this->file_of("porch"), R"({"id":"porch","name":"Porch","kind":"fan"})");
  const std::string text = line_of(calibrated(1, "Winter")) + "\n";
  this->put_sets("porch", text);
  this->reboot();
  EXPECT_EQ(nullptr, hub().store().get("porch"));
  Result result;
  EXPECT_EQ(nullptr, hub().open_gain_sets("porch", &result));
  EXPECT_EQ(404, result.code);
  EXPECT_EQ(404, hub().add_gain_set("porch", nullopt, "").code);
  EXPECT_EQ(text, read_file(this->sets_of("porch")));
}

// --- Saving one by hand ---

TEST_F(GainSets, AddSavesTheGainsOfTheModesRelay) {
  const std::string id = this->create(studio()).id;
  const GainSet cool = add(id);
  EXPECT_EQ(1u, cool.n);
  EXPECT_EQ(Direction::COOL, cool.direction) << "the mode's relay";
  EXPECT_EQ(0.75f, cool.gains.kp);
  EXPECT_EQ(0.5f, cool.gains.ki);
  EXPECT_EQ(GAIN_SET_MANUAL, cool.source);
  EXPECT_EQ("", cool.rule);
  EXPECT_FALSE(cool.has_measurement());
  EXPECT_TRUE(cool.flags.empty());
  EXPECT_FALSE(cool.time.has_value()) << "no clock";
  EXPECT_FALSE(cool.from.has_value());
  EXPECT_EQ("Cool manual #1", cool.name);

  hub().clock = ESPTime::from_epoch_utc(SAVED_AT);
  const GainSet heat = add(id, Direction::HEAT, "  Winter  ");
  EXPECT_EQ(2u, heat.n);
  EXPECT_EQ(Direction::HEAT, heat.direction);
  EXPECT_EQ(1.5f, heat.gains.kp);
  EXPECT_EQ(2.f, heat.gains.kd);
  EXPECT_EQ("Winter", heat.name);
  ASSERT_TRUE(heat.time.has_value());
  EXPECT_EQ(SAVED_AT, *heat.time);
  EXPECT_EQ("2026-11-06 14:05 manual", add(id, Direction::HEAT).name);
  EXPECT_EQ("Winter", add(id, Direction::HEAT, "Winter").name) << "names may repeat";

  const std::vector<GainSet> listed = sets(id);
  ASSERT_EQ(4u, listed.size());
  EXPECT_EQ(line_of(heat), line_of(listed[1]));
  // Nothing about the thermostat moved.
  EXPECT_EQ(0u, hub().store().get(id)->revision);
  EXPECT_TRUE(list_dir(this->folder()) == (std::vector<std::string>{"gains", "studio.json"}));
}

TEST_F(GainSets, AddIsRefusedWithTheSentenceThatSaysWhy) {
  const std::string id = this->start();
  struct Refusal {
    Result result;
    uint16_t code;
    const char *error;
  };
  hub().free_bytes = GAIN_SET_MIN_FREE_BYTES - 1;
  for (const Refusal &refusal : {
           Refusal{hub().add_gain_set(id, nullopt, std::string(49, 'a')), 400, "Name is longer than 48 characters"},
           Refusal{hub().add_gain_set("attic", nullopt, std::string(49, 'a')), 400,
                   "Name is longer than 48 characters"},
           Refusal{hub().add_gain_set("attic", nullopt, ""), 404, "Thermostat not found"},
           Refusal{hub().add_gain_set(id, Direction::COOL, ""), 409, "The thermostat has no cooling relay"},
           Refusal{hub().add_gain_set(id, nullopt, ""), 507,
                   "Less than 64 KiB is free on the storage; delete gain sets or files to make room for another"},
       }) {
    EXPECT_FALSE(refusal.result.ok);
    EXPECT_EQ(refusal.code, refusal.result.code) << refusal.error;
    EXPECT_EQ(refusal.error, refusal.result.error);
  }
  EXPECT_FALSE(file_exists(this->gains()));
  hub().free_bytes = GAIN_SET_MIN_FREE_BYTES;
  EXPECT_TRUE(hub().add_gain_set(id, nullopt, "").ok) << "at the limit";
  hub().free_bytes.reset();

  // A file where the folder goes: nothing can be written.
  ::remove(this->sets_of(id).c_str());
  rmdir(this->gains().c_str());
  write_file(this->gains(), "");
  LogCapture::instance().clear();
  const Result unwritten = hub().add_gain_set(id, nullopt, "");
  EXPECT_EQ(500, unwritten.code);
  EXPECT_EQ("The gain sets file could not be written", unwritten.error);
  EXPECT_TRUE(LogCapture::instance().has("Cannot write"));
  ::remove(this->gains().c_str());
}

TEST_F(GainSets, RenameAndDelete) {
  const std::string id = this->start();
  add(id, nullopt, "One");
  add(id, nullopt, "Two");
  GainSet renamed;
  ASSERT_TRUE(hub().rename_gain_set(id, 2, " Зимний пол ", &renamed).ok);
  EXPECT_EQ(2u, renamed.n);
  EXPECT_EQ("Зимний пол", renamed.name);
  EXPECT_EQ("Зимний пол", sets(id)[1].name);
  this->reboot();
  EXPECT_EQ("Зимний пол", sets(id)[1].name);

  EXPECT_EQ("Name is required", hub().rename_gain_set(id, 2, "   ").error);
  EXPECT_EQ(404, hub().rename_gain_set("attic", 2, "x").code);
  EXPECT_EQ("Gain set not found", hub().rename_gain_set(id, 7, "x").error);
  EXPECT_EQ("Gain set not found", hub().delete_gain_set(id, 7).error);
  EXPECT_EQ("Thermostat not found", hub().delete_gain_set("attic", 1).error);
  ASSERT_TRUE(hub().delete_gain_set(id, 1).ok);
  EXPECT_EQ(std::vector<uint32_t>{2}, numbers(id));

  // A thermostat with no sets has no file to rewrite.
  const std::string other = this->create(draft("Floor", "relay_2")).id;
  EXPECT_EQ(404, hub().delete_gain_set(other, 1).code);
  EXPECT_EQ(404, hub().rename_gain_set(other, 1, "x").code);

  // A rewrite that cannot be written changes nothing.
  mkdir((this->sets_of(id) + ".tmp").c_str(), 0755);
  const std::string text = read_file(this->sets_of(id));
  EXPECT_EQ(500, hub().rename_gain_set(id, 2, "x").code);
  EXPECT_EQ("The gain sets file could not be written", hub().delete_gain_set(id, 2).error);
  EXPECT_EQ(text, read_file(this->sets_of(id)));
  rmdir((this->sets_of(id) + ".tmp").c_str());
}

// --- Applying one ---

TEST_F(GainSets, ApplyMakesTheGainsTheRelaysAndMovesTheRevision) {
  const std::string id = this->start();
  const GainSet saved = add(id);
  ClimateConfig doc = *hub().store().get(id);
  doc.pid.heat = PidGains{2.f, 0.001f, 5.f};
  ASSERT_TRUE(hub().update(id, doc, 0u).ok);
  // A few passes for the PID to learn something.
  for (int i = 0; i < 4; i++) {
    hub().ms += 30000;
    entities().room.publish_state(18.f);
    hub().loop();
  }
  ControllerRuntime *rt = hub().runtime_of(id);
  const float learnt = rt->pid().integral_term();
  ASSERT_GT(learnt, 0.f);

  const Result applied = hub().apply_gain_set(id, saved.n);
  ASSERT_TRUE(applied.ok) << applied.error;
  EXPECT_TRUE(applied.persisted);
  const ClimateConfig *stored = hub().store().get(id);
  EXPECT_EQ(0.6f, stored->pid.heat.kp);
  EXPECT_EQ(0.0025f, stored->pid.heat.ki);
  EXPECT_EQ(0.f, stored->pid.heat.kd);
  EXPECT_EQ(1u, stored->revision);
  ClimateConfig flash;
  std::string error;
  ASSERT_TRUE(from_json(read_file(this->file_of(id)), &flash, &error)) << error;
  EXPECT_EQ(0.6f, flash.pid.heat.kp);
  EXPECT_EQ(1u, flash.revision);

  // On as after a Save: the new gains, and what it had learnt.
  EXPECT_EQ(rt, hub().runtime_of(id));
  hub().ms += 30000;
  entities().room.publish_state(18.f);
  hub().loop();
  EXPECT_FLOAT_EQ(0.6f * rt->pid().error(), rt->pid().proportional_term());
  EXPECT_GT(rt->pid().integral_term(), learnt);

  // A form read before the apply would write the old gains back.
  const Result stale = hub().update(id, doc, 0u);
  EXPECT_EQ(409, stale.code);
  EXPECT_EQ("The device changed this thermostat since it was read; reload it", stale.error);
  EXPECT_TRUE(hub().update(id, *stored, 1u).ok);
  EXPECT_EQ(std::vector<uint32_t>{1}, numbers(id)) << "the set stays";
}

TEST_F(GainSets, ApplyGoesIntoTheSetsRelayAlone) {
  const std::string id = this->create(studio()).id;
  this->put_sets(id, R"({"n":1,"name":"Hand","direction":"heat","kp":5000,"ki":-1,"kd":3})"
                     "\n");
  ASSERT_TRUE(hub().apply_gain_set(id, 1).ok);
  const ClimateConfig *stored = hub().store().get(id);
  EXPECT_EQ(find_param("kp")->max, stored->pid.heat.kp) << "held in range, as a Save holds it";
  EXPECT_EQ(0.f, stored->pid.heat.ki);
  EXPECT_EQ(3.f, stored->pid.heat.kd);
  EXPECT_EQ(0.75f, stored->pid.cool.kp) << "the other relay keeps its own";
}

TEST_F(GainSets, ApplyIsRefusedWithTheSentenceThatSaysWhy) {
  const std::string id = this->start();
  add(id);
  EXPECT_EQ("Thermostat not found", hub().apply_gain_set("attic", 1).error);
  EXPECT_EQ("Gain set not found", hub().apply_gain_set(id, 2).error);

  ASSERT_TRUE(hub().start_autotune(id, nullopt, AutotuneRule::ZN_PI).ok);
  const Result calibrating = hub().apply_gain_set(id, 1);
  EXPECT_EQ(409, calibrating.code);
  EXPECT_EQ("A calibration is running; apply a gain set once it ends or is cancelled", calibrating.error);
  EXPECT_TRUE(hub().autotune(id)->running()) << "and it runs on";
  ASSERT_TRUE(hub().cancel_autotune(id).ok);

  // A set saved while the thermostat had a cooling relay, which a Save took away since.
  ASSERT_TRUE(hub().set_enabled(id, false).ok);
  ClimateConfig both = studio();
  const std::string studio_id = this->create(both).id;
  add(studio_id);
  both = *hub().store().get(studio_id);
  both.cool.relay_id = "";
  both.mode = HubMode::HEAT;
  ASSERT_TRUE(hub().update(studio_id, both).ok);
  const Result no_relay = hub().apply_gain_set(studio_id, 1);
  EXPECT_EQ(409, no_relay.code);
  EXPECT_EQ("The thermostat has no cooling relay", no_relay.error);
  EXPECT_EQ(0u, hub().store().get(studio_id)->revision);

  // A newer firmware's file is never written.
  std::string text = read_file(this->file_of(id));
  text.replace(text.find("\"version\":5"), 11, "\"version\":9");
  write_file(this->file_of(id), text);
  this->reboot();
  const Result newer = hub().apply_gain_set(id, 1);
  EXPECT_EQ(409, newer.code);
  EXPECT_EQ("A newer firmware wrote this thermostat; update the firmware to change it", newer.error);
  EXPECT_TRUE(hub().add_gain_set(id, nullopt, "").ok) << "the sets are not the thermostat's file";
}

// The file a Save could not write takes the gains all the same, as a calibration's: they run, and
// the next flush writes them.
TEST_F(GainSets, AnApplyTheFileDidNotTakeRunsAndIsWrittenLater) {
  const std::string id = this->start();
  this->put_sets(id, R"({"n":1,"name":"Hand","direction":"heat","kp":3,"ki":0.002,"kd":0})"
                     "\n");
  hub().max_file_bytes = 100;
  const Result applied = hub().apply_gain_set(id, 1);
  ASSERT_TRUE(applied.ok);
  EXPECT_FALSE(applied.persisted);
  EXPECT_TRUE(hub().dirty(id));
  EXPECT_EQ(3.f, hub().store().get(id)->pid.heat.kp);
  hub().max_file_bytes = CONFIG_MAX_BYTES;
  hub().ms += 4000;
  hub().loop();
  EXPECT_FALSE(hub().dirty(id));
  this->reboot();
  EXPECT_EQ(3.f, hub().store().get(id)->pid.heat.kp);
  EXPECT_EQ(1u, hub().store().get(id)->revision);
}

// --- Deriving one ---

TEST_F(GainSets, DeriveGivesAnotherRulesGainsForTheSameKuAndPu) {
  const std::string id = this->start();
  this->put_sets(id, std::string(MEASURED) + "\n");
  GainSet derived;
  const Result result = hub().derive_gain_set(id, 1, AutotuneRule::ZN_PID, false, "", &derived);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(2u, derived.n);
  EXPECT_EQ(Direction::HEAT, derived.direction);
  EXPECT_EQ(GAIN_SET_DERIVED, derived.source);
  EXPECT_EQ("zn_pid", derived.rule);
  EXPECT_EQ(10.f, derived.ku);
  EXPECT_EQ(1200.f, derived.pu);
  bool clamped = true;
  const PidGains want = gains_for_rule(10.f, 1200.f, AutotuneRule::ZN_PID, &clamped);
  EXPECT_FALSE(clamped);
  EXPECT_EQ(as_stored(want.kp), derived.gains.kp);
  EXPECT_EQ(as_stored(want.ki), derived.gains.ki);
  EXPECT_EQ(as_stored(want.kd), derived.gains.kd);
  EXPECT_FLOAT_EQ(6.f, derived.gains.kp);
  EXPECT_FLOAT_EQ(0.01f, derived.gains.ki);
  EXPECT_FLOAT_EQ(900.f, derived.gains.kd);
  EXPECT_EQ(std::vector<std::string>{"uneven"}, derived.flags) << "the measurement's warning, not the old clamp";
  ASSERT_TRUE(derived.from.has_value());
  EXPECT_EQ(1u, *derived.from);
  EXPECT_EQ("Heat ZN PID #2", derived.name);
  EXPECT_EQ(0u, hub().store().get(id)->revision) << "not applied";
  EXPECT_EQ(0.6f, hub().store().get(id)->pid.heat.kp);
  const std::vector<GainSet> listed = sets(id);
  ASSERT_EQ(2u, listed.size());
  EXPECT_EQ(std::string(MEASURED), line_of(listed[0])) << "the parent is untouched";
  EXPECT_EQ(line_of(derived), line_of(listed[1]));

  // A derived set has Ku and Pu too; a gain past its range is the new rule's clamp.
  hub().clock = ESPTime::from_epoch_utc(SAVED_AT);
  this->put_sets(id, std::string(MEASURED) + "\n" +
                         R"({"n":2,"name":"Slow","direction":"heat","source":"derived","rule":"zn_pi","ku":10,)"
                         R"("pu":360000,"kp":4.5,"ki":0,"kd":0,"flags":[],"from":1})"
                         "\n");
  ASSERT_TRUE(hub().derive_gain_set(id, 2, AutotuneRule::SOME_OVERSHOOT, false, "Floor, gentle", &derived).ok);
  EXPECT_EQ(3u, derived.n);
  EXPECT_EQ("Floor, gentle", derived.name);
  EXPECT_EQ(std::vector<std::string>{"clamped"}, derived.flags);
  EXPECT_EQ(find_param("kd")->max, derived.gains.kd);
  EXPECT_EQ(2u, *derived.from);
  ASSERT_TRUE(derived.time.has_value());
  ASSERT_TRUE(hub().derive_gain_set(id, 1, AutotuneRule::PESSEN, false, "", &derived).ok);
  EXPECT_EQ("2026-11-06 14:05 Pessen", derived.name);
}

TEST_F(GainSets, DeriveAndApplyIsOneStep) {
  const std::string id = this->start();
  this->put_sets(id, std::string(MEASURED) + "\n");
  GainSet derived;
  const Result result = hub().derive_gain_set(id, 1, AutotuneRule::NO_OVERSHOOT, true, "", &derived);
  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.persisted);
  const ClimateConfig *stored = hub().store().get(id);
  EXPECT_EQ(1u, stored->revision);
  EXPECT_EQ(derived.gains.kp, stored->pid.heat.kp);
  EXPECT_FLOAT_EQ(2.f, stored->pid.heat.kp);
  EXPECT_EQ(as_stored(0.0625f * 10.f * 1200.f), stored->pid.heat.kd);

  // Refused for the apply before anything is added.
  ASSERT_TRUE(hub().start_autotune(id, nullopt, AutotuneRule::ZN_PI).ok);
  const Result calibrating = hub().derive_gain_set(id, 1, AutotuneRule::ZN_PID, true, "", nullptr);
  EXPECT_EQ(409, calibrating.code);
  EXPECT_EQ((std::vector<uint32_t>{1, 2}), numbers(id));
  EXPECT_TRUE(hub().derive_gain_set(id, 1, AutotuneRule::ZN_PID, false, "", nullptr).ok) << "a set alone is no apply";
}

TEST_F(GainSets, DeriveIsRefusedWithTheSentenceThatSaysWhy) {
  const std::string id = this->start();
  add(id);
  std::string measured = MEASURED;
  measured.replace(measured.find("\"n\":1"), 5, "\"n\":2");
  this->put_sets(id, read_file(this->sets_of(id)) + measured + "\n");
  EXPECT_EQ("This set has no Ku and Pu", hub().derive_gain_set(id, 1, AutotuneRule::ZN_PID, false, "", nullptr).error);
  EXPECT_EQ(404, hub().derive_gain_set("attic", 2, AutotuneRule::ZN_PID, false, "", nullptr).code);
  EXPECT_EQ("Gain set not found", hub().derive_gain_set(id, 9, AutotuneRule::ZN_PID, false, "", nullptr).error);
  EXPECT_EQ("Name cannot contain control characters",
            hub().derive_gain_set(id, 2, AutotuneRule::ZN_PID, false, "a\nb", nullptr).error);
  hub().free_bytes = 1024;
  EXPECT_EQ(507, hub().derive_gain_set(id, 2, AutotuneRule::ZN_PID, false, "", nullptr).code);
  EXPECT_EQ((std::vector<uint32_t>{1, 2}), numbers(id)) << "nothing added";
}

// --- A calibration's own ---

TEST_F(GainSets, ACalibrationSavesItsResultAsASet) {
  hub().clock = ESPTime::from_epoch_utc(SAVED_AT);
  const std::string id = this->start();
  calibrate(id);
  const AutotuneRun *run = hub().autotune(id);
  ASSERT_EQ(AutotuneState::SUCCEEDED, run->state());
  EXPECT_EQ(1u, run->saved_n());
  EXPECT_EQ("2026-11-06 14:05 ZN PI", run->saved_name());
  const std::vector<GainSet> listed = sets(id);
  ASSERT_EQ(1u, listed.size());
  const GainSet &set = listed[0];
  EXPECT_EQ(GAIN_SET_CALIBRATION, set.source);
  EXPECT_EQ("zn_pi", set.rule);
  EXPECT_EQ(Direction::HEAT, set.direction);
  EXPECT_EQ(as_stored(run->tuner().ku()), set.ku);
  EXPECT_EQ(as_stored(run->tuner().pu()), set.pu);
  EXPECT_EQ(run->new_gains().kp, set.gains.kp) << "what the thermostat runs on";
  EXPECT_EQ(run->new_gains().ki, set.gains.ki);
  EXPECT_EQ(hub().store().get(id)->pid.heat.kp, set.gains.kp);
  EXPECT_EQ(run->uneven(), set.has_flag("uneven"));
  EXPECT_EQ(run->asymmetric(), set.has_flag("asymmetric"));
  ASSERT_TRUE(set.time.has_value());
  EXPECT_EQ(SAVED_AT, *set.time);

  // The status follows the set through a rename, and lets go of it when it is deleted.
  ASSERT_TRUE(hub().rename_gain_set(id, 1, "Radiators").ok);
  EXPECT_EQ("Radiators", hub().autotune(id)->saved_name());
  ASSERT_TRUE(hub().delete_gain_set(id, 1).ok);
  EXPECT_EQ(0u, hub().autotune(id)->saved_n());
}

TEST_F(GainSets, ACalibrationWithoutAClockNamesItsSetByItsRelayAndNumber) {
  const std::string id = this->start();
  add(id);
  calibrate(id);
  const std::vector<GainSet> listed = sets(id);
  ASSERT_EQ(2u, listed.size());
  EXPECT_EQ("Heat ZN PI #2", listed[1].name);
  EXPECT_FALSE(listed[1].time.has_value());
  EXPECT_EQ(2u, hub().autotune(id)->saved_n());
}

// The run's warnings are the set's: whoever applies it later sees them.
TEST_F(GainSets, ACalibrationsWarningsGoWithItsSet) {
  ClimateConfig config = draft("Living room");
  // The long half-period is longer than the default timeout.
  config.safety.sensor_timeout_s = 3600.f;
  const std::string id = this->start(config, 21.f);
  ASSERT_TRUE(hub().start_autotune(id, nullopt, AutotuneRule::ZN_PI).ok);
  // Two short half-periods and a long one, a small swing among large ones.
  const std::pair<uint32_t, float> readings[] = {{0, 21.f},    {60, 20.7f}, {120, 21.3f}, {180, 20.7f},
                                                 {900, 21.9f}, {960, 20.f}, {1020, 21.3f}};
  const uint64_t start = hub().ms;
  for (const auto &reading : readings) {
    hub().ms = start + 1000 + reading.first * 1000ull;
    hub().loop();
    entities().room.publish_state(reading.second);
  }
  const AutotuneRun *run = hub().autotune(id);
  ASSERT_EQ(AutotuneState::SUCCEEDED, run->state());
  ASSERT_TRUE(run->asymmetric());
  ASSERT_TRUE(run->uneven());
  const std::vector<GainSet> listed = sets(id);
  ASSERT_EQ(1u, listed.size());
  EXPECT_EQ((std::vector<std::string>{"asymmetric", "uneven"}), listed[0].flags);
}

// Below the free space a set needs, the gains go into the thermostat all the same.
TEST_F(GainSets, ACalibrationOnAFullStorageSavesNoSet) {
  const std::string id = this->start();
  hub().free_bytes = 4096;
  LogCapture::instance().clear();
  calibrate(id);
  const AutotuneRun *run = hub().autotune(id);
  ASSERT_EQ(AutotuneState::SUCCEEDED, run->state());
  EXPECT_EQ(0u, run->saved_n());
  EXPECT_TRUE(run->persisted());
  EXPECT_EQ(run->new_gains().kp, hub().store().get(id)->pid.heat.kp);
  EXPECT_TRUE(LogCapture::instance().has("the calibration was not saved as a gain set: Less than 64 KiB is free"));
  EXPECT_FALSE(file_exists(this->sets_of(id)));
}

// A set that waited for a reader and then could not be written is not named by the status.
TEST_F(GainSets, ASetThatWaitedAndCouldNotBeWrittenIsNotNamed) {
  const std::string id = this->start();
  Result result;
  auto reader = hub().open_gain_sets(id, &result);
  calibrate(id);
  ASSERT_EQ(1u, hub().autotune(id)->saved_n());
  ASSERT_FALSE(file_exists(this->gains()));
  write_file(this->gains(), "");
  reader.reset();
  LogCapture::instance().clear();
  hub().loop();
  EXPECT_TRUE(LogCapture::instance().has("'living-room': gain set 1 was not written"));
  EXPECT_EQ(0u, hub().autotune(id)->saved_n());
  ::remove(this->gains().c_str());
}

TEST_F(GainSets, EveryChangeIsRefusedWhileTheStorageFailed) {
  const std::string id = this->start();
  add(id);
  hub().mark_failed();
  for (const Result &result :
       {hub().add_gain_set(id, nullopt, ""), hub().rename_gain_set(id, 1, "x"), hub().delete_gain_set(id, 1),
        hub().apply_gain_set(id, 1), hub().derive_gain_set(id, 1, AutotuneRule::ZN_PI, false, "", nullptr)}) {
    EXPECT_EQ(500, result.code);
    EXPECT_EQ("Thermostat storage is not available", result.error);
  }
  hub().reset_to_construction_state();
}

}  // namespace esphome::climate_hub::testing
