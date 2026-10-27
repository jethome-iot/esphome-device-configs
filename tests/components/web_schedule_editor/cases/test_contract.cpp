#include <set>
#include "common.h"

// Runs ../contract.json, which the dashboard runs against its client mock too: see doc/TESTING.md.
namespace esphome::web_schedule_editor::testing {
namespace {

JsonDocument load_contract() {
  // run.py runs the suite from its own directory.
  std::ifstream in("contract.json");
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  JsonDocument doc;
  EXPECT_FALSE(text.empty()) << "contract.json not found: run the suite from tests/components/web_schedule_editor";
  // A case nests a step, its expectations and a document's modes, actions and values.
  EXPECT_EQ(deserializeJson(doc, text, DeserializationOption::NestingLimit(16)), DeserializationError::Ok);
  return doc;
}

// The keys this runner reads; a case's and a step's are the dashboard runner's too. Anything
// else, a typo too, would pass having checked nothing.
const std::set<std::string> TOP_KEYS{"$comment", "environment", "fixtures", "cases"};
const std::set<std::string> ENVIRONMENT_KEYS{"max_schedules", "now", "relays", "thermostats", "missing"};
const std::set<std::string> CLOCK_KEYS{"day", "time"};
const std::set<std::string> RELAY_KEYS{"object_id", "name"};
const std::set<std::string> THERMOSTAT_KEYS{
    "id", "name", "enabled", "running", "mode", "min_temperature", "max_temperature", "step", "presets"};
const std::set<std::string> STEP_KEYS{"method", "path",         "body",    "pad_to", "status",
                                      "error",  "error_prefix", "headers", "expect", "absent"};
// What `newer_file` writes: a literal the contract can expect, far enough ahead to outlast bumps.
constexpr unsigned NEWER_VERSION = 9;
static_assert(schedules::SCHEDULE_VERSION < NEWER_VERSION, "newer_file must stay newer than this firmware");

const std::set<std::string> CASE_KEYS = [] {
  std::set<std::string> keys = STEP_KEYS;
  keys.insert({"name", "setup", "device_only", "clock", "then"});
  return keys;
}();

void refuse_unknown_keys(JsonObjectConst object, const std::set<std::string> &known, const std::string &where) {
  for (JsonPairConst member : object) {
    if (!known.contains(member.key().c_str()))
      ADD_FAILURE() << where << ": \"" << member.key().c_str() << "\" is a key this runner does not check";
  }
}

std::string text_of(JsonVariantConst value) {
  std::string out;
  serializeJson(value, out);
  return out;
}

// The member or element `segment` names; `found` is false when there is none.
JsonVariantConst step_into(JsonVariantConst at, const std::string &segment, bool *found) {
  *found = false;
  if (at.is<JsonArrayConst>()) {
    JsonArrayConst array = at.as<JsonArrayConst>();
    const size_t eq = segment.find('=');
    if (eq != std::string::npos) {
      const std::string key = segment.substr(0, eq);
      const std::string value = segment.substr(eq + 1);
      for (JsonVariantConst element : array) {
        JsonVariantConst field = element[key];
        if (field.is<const char *>() && field.as<std::string>() == value) {
          *found = true;
          return element;
        }
      }
      return {};
    }
    if (segment.empty() || segment.find_first_not_of("0123456789") != std::string::npos)
      return {};
    const size_t index = std::stoul(segment);
    if (index >= array.size())
      return {};
    *found = true;
    return array[index];
  }
  if (at.is<JsonObjectConst>()) {
    for (JsonPairConst member : at.as<JsonObjectConst>()) {
      if (segment == member.key().c_str()) {
        *found = true;
        return member.value();
      }
    }
  }
  return {};
}

JsonVariantConst find_path(JsonVariantConst root, const std::string &path, bool *found) {
  JsonVariantConst at = root;
  size_t start = 0;
  for (;;) {
    const size_t dot = path.find('.', start);
    at = step_into(at, path.substr(start, dot == std::string::npos ? std::string::npos : dot - start), found);
    if (!*found || dot == std::string::npos)
      return at;
    start = dot + 1;
  }
}

// JSON equality, numbers to a float's precision: the device writes floats.
bool same(JsonVariantConst expected, JsonVariantConst actual) {
  if (expected.isNull())
    return actual.isNull();
  if (expected.is<bool>())
    return actual.is<bool>() && expected.as<bool>() == actual.as<bool>();
  if (expected.is<double>()) {
    if (!actual.is<double>())
      return false;
    const double e = expected.as<double>();
    return std::fabs(e - actual.as<double>()) <= 1e-6 * std::max(1.0, std::fabs(e));
  }
  if (expected.is<const char *>())
    return actual.is<const char *>() && expected.as<std::string>() == actual.as<std::string>();
  if (expected.is<JsonArrayConst>()) {
    JsonArrayConst want = expected.as<JsonArrayConst>();
    JsonArrayConst got = actual.as<JsonArrayConst>();
    if (!actual.is<JsonArrayConst>() || want.size() != got.size())
      return false;
    for (size_t i = 0; i < want.size(); i++) {
      if (!same(want[i], got[i]))
        return false;
    }
    return true;
  }
  if (expected.is<JsonObjectConst>()) {
    if (!actual.is<JsonObjectConst>() || expected.size() != actual.size())
      return false;
    for (JsonPairConst member : expected.as<JsonObjectConst>()) {
      bool found = false;
      JsonVariantConst value = step_into(actual, member.key().c_str(), &found);
      if (!found || !same(member.value(), value))
        return false;
    }
    return true;
  }
  return false;
}

http_method method_of(const std::string &name) {
  if (name == "GET")
    return HTTP_GET;
  if (name == "POST")
    return HTTP_POST;
  EXPECT_EQ(name, "OPTIONS") << "a method the device's server hands over";
  return HTTP_OPTIONS;
}

// The route a path names, without its query.
std::string route_of(const std::string &path) { return path.substr(0, path.find('?')); }

// {"day": "wed", "time": "10:15:42"} as local seconds; 0, no valid clock, for null.
int64_t clock_of(JsonVariantConst clock) {
  if (clock.isNull())
    return 0;
  const std::string day = clock["day"] | "";
  int index = -1;
  for (int i = 0; i < 7; i++) {
    if (day == schedules::DAY_NAMES[i])
      index = i;
  }
  EXPECT_GE(index, 0) << "no day " << day;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  const std::string time = clock["time"] | "";
  EXPECT_EQ(sscanf(time.c_str(), "%2u:%2u:%2u", &hour, &minute, &second), 3) << time;
  return at_local(index, static_cast<int>(hour), static_cast<int>(minute), static_cast<int>(second));
}

class Contract : public Editor {
 protected:
  void put_into(const std::string &state) {
    if (state == "loop_busy") {
      engine().loop_busy = true;
    } else if (state == "storage_failed") {
      engine().mark_failed();
    } else if (state == "storage_unwritable") {
      storage().path = "/proc/definitely-not-writable";
    } else if (state == "file_cap") {
      engine().max_file_bytes = 100;
    } else if (state == "newer_file") {
      this->reboot_with_version(NEWER_VERSION);
    } else if (state == "relay_held") {
      // contract.json's relays hold none of the thermostats', so one is held in their place.
      fake_holder().held = &this->e.relay3;
      fake_holder().name = "Floor";
      switch_hold::set_holder(&fake_holder());
    } else {
      ADD_FAILURE() << "unknown device_only: " << state;
    }
  }

  void take_out_of(const std::string &state) {
    engine().loop_busy = false;
    engine().reset_to_construction_state();
    storage().path = this->base_path;
    engine().max_file_bytes = schedules::MAX_FILE_BYTES;
    switch_hold::set_holder(&hub());
    // newer_file stays: the files are the newer firmware's, and what a case changed in memory
    // is what its `then` reads. The next case starts over from an empty folder.
  }

  // Every file in the folder rewritten with this version, and read by a boot.
  void reboot_with_version(unsigned version) {
    for (const std::string &name : this->files()) {
      JsonDocument file;
      ASSERT_EQ(deserializeJson(file, this->file(name)), DeserializationError::Ok) << name;
      file["version"] = version;
      std::string text;
      serializeJson(file, text);
      std::ofstream(this->folder() + "/" + name) << text;
    }
    this->reboot();
  }

  // Sends one request of the contract's and checks its answer against it. The loop ticks first,
  // as the mock reads its clock first.
  void check(JsonObjectConst step) {
    engine().tick();
    engine().run_later();
    std::string body;
    JsonVariantConst raw = step["body"];
    if (raw.is<const char *>()) {
      body = raw.as<std::string>();
    } else if (!raw.isNull()) {
      serializeJson(raw, body);
    }
    if (step["pad_to"].is<int>())
      body.resize(std::max(body.size(), step["pad_to"].as<size_t>()), ' ');
    const std::string path = step["path"].as<std::string>();
    Reply reply = this->request(method_of(step["method"].as<std::string>()), "/schedule-editor/api/" + path, body);
    engine().run_later();
    const std::string shown = reply.body.size() > 400 ? reply.body.substr(0, 400) + "..." : reply.body;
    SCOPED_TRACE(step["method"].as<std::string>() + " " + path + " answered " + shown);
    EXPECT_EQ(reply.code, step["status"].as<int>());

    JsonVariantConst answer = reply.json.as<JsonVariantConst>();
    if (step["error"].is<const char *>() || step["error_prefix"].is<const char *>()) {
      EXPECT_FALSE(reply["success"] | true);
      const std::string error = reply.error();
      if (step["error"].is<const char *>())
        EXPECT_EQ(error, step["error"].as<std::string>());
      if (step["error_prefix"].is<const char *>())
        EXPECT_TRUE(error.starts_with(step["error_prefix"].as<std::string>())) << error;
    }
    for (JsonPairConst header : step["headers"].as<JsonObjectConst>())
      EXPECT_EQ(reply.header(header.key().c_str()), header.value().as<std::string>()) << header.key().c_str();
    for (JsonPairConst want : step["expect"].as<JsonObjectConst>()) {
      bool found = false;
      JsonVariantConst got = find_path(answer, want.key().c_str(), &found);
      EXPECT_TRUE(found) << want.key().c_str() << " is not in the answer";
      if (found) {
        EXPECT_TRUE(same(want.value(), got))
            << want.key().c_str() << ": " << text_of(got) << ", not " << text_of(want.value());
      }
    }
    for (JsonVariantConst path : step["absent"].as<JsonArrayConst>()) {
      bool found = false;
      find_path(answer, path.as<std::string>(), &found);
      EXPECT_FALSE(found) << path.as<std::string>() << " is in the answer";
    }
  }
};

}  // namespace

// The file and this suite describe one device: the limit, the clock, the relays and the
// thermostats its cases name are here, as /targets lists them, and nothing it calls missing is.
TEST_F(Contract, TheContractsDeviceIsThisSuites) {
  JsonDocument contract = load_contract();
  JsonObjectConst environment = contract["environment"];
  EXPECT_EQ(engine().max_schedules(), environment["max_schedules"].as<int>());
  EXPECT_EQ(clock_of(environment["now"]), wednesday());

  Reply targets = this->get("targets");
  ASSERT_EQ(targets.code, 200);
  for (JsonObjectConst want : environment["relays"].as<JsonArrayConst>()) {
    bool found = false;
    JsonVariantConst got = find_path(targets.json.as<JsonVariantConst>(),
                                     "relays.object_id=" + want["object_id"].as<std::string>(), &found);
    ASSERT_TRUE(found) << want["object_id"].as<std::string>();
    EXPECT_EQ(want["name"].as<std::string>(), got["name"].as<std::string>());
    EXPECT_EQ("", got["held_by"].as<std::string>()) << "a relay the contract names is held by none";
  }
  for (JsonObjectConst want : environment["thermostats"].as<JsonArrayConst>()) {
    const std::string id = want["id"].as<std::string>();
    bool found = false;
    JsonVariantConst got = find_path(targets.json.as<JsonVariantConst>(), "thermostats.id=" + id, &found);
    ASSERT_TRUE(found) << id;
    for (JsonPairConst field : want)
      EXPECT_TRUE(same(field.value(), got[field.key()])) << id << "." << field.key().c_str() << ": " << text_of(got);
  }
  for (JsonVariantConst id : environment["missing"].as<JsonArrayConst>()) {
    bool found = false;
    find_path(targets.json.as<JsonVariantConst>(), "relays.object_id=" + id.as<std::string>(), &found);
    EXPECT_FALSE(found) << id.as<std::string>();
    find_path(targets.json.as<JsonVariantConst>(), "thermostats.id=" + id.as<std::string>(), &found);
    EXPECT_FALSE(found) << id.as<std::string>();
  }
}

// Every status the API answers, and a success of every route, is in the file.
TEST_F(Contract, TheContractCoversEveryStatusAndEveryRoute) {
  JsonDocument contract = load_contract();
  std::set<int> statuses;
  std::set<std::string> answered;
  std::set<std::string> names;
  for (JsonObjectConst c : contract["cases"].as<JsonArrayConst>()) {
    statuses.insert(c["status"].as<int>());
    if (c["status"].as<int>() == 200)
      answered.insert(route_of(c["path"].as<std::string>()));
    EXPECT_TRUE(names.insert(c["name"].as<std::string>()).second) << "twice: " << c["name"].as<std::string>();
  }
  for (int status : {400, 404, 405, 409, 413, 500, 503, 507})
    EXPECT_EQ(statuses.count(status), 1u) << status;
  for (const char *route : {"list", "get", "targets", "schema", "ping", "save", "import", "delete", "enable"})
    EXPECT_EQ(answered.count(route), 1u) << route;
}

TEST_F(Contract, EveryKeyIsOneTheRunnersRead) {
  JsonDocument contract = load_contract();
  refuse_unknown_keys(contract.as<JsonObjectConst>(), TOP_KEYS, "the file");
  JsonObjectConst environment = contract["environment"];
  refuse_unknown_keys(environment, ENVIRONMENT_KEYS, "environment");
  refuse_unknown_keys(environment["now"], CLOCK_KEYS, "environment, now");
  for (JsonObjectConst relay : environment["relays"].as<JsonArrayConst>())
    refuse_unknown_keys(relay, RELAY_KEYS, "environment, a relay");
  for (JsonObjectConst thermostat : environment["thermostats"].as<JsonArrayConst>())
    refuse_unknown_keys(thermostat, THERMOSTAT_KEYS, "environment, a thermostat");
  for (JsonObjectConst c : contract["cases"].as<JsonArrayConst>()) {
    const std::string name = c["name"] | "a case with no name";
    refuse_unknown_keys(c, CASE_KEYS, name);
    if (c["clock"].is<JsonObjectConst>())
      refuse_unknown_keys(c["clock"], CLOCK_KEYS, name + ", clock");
    int index = 0;
    for (JsonObjectConst then : c["then"].as<JsonArrayConst>())
      refuse_unknown_keys(then, STEP_KEYS, name + ", then #" + std::to_string(++index));
  }
}

TEST_F(Contract, EveryContractCaseGetsItsAnswer) {
  JsonDocument contract = load_contract();
  JsonObjectConst fixtures = contract["fixtures"];
  JsonVariantConst now = contract["environment"]["now"];
  size_t ran = 0;
  for (JsonObjectConst c : contract["cases"].as<JsonArrayConst>()) {
    SCOPED_TRACE(c["name"].as<std::string>());
    const std::string state = c["device_only"] | "";
    // A case's clock holds for the whole case; null is a clock not valid yet.
    this->fresh(clock_of(c["clock"].isUnbound() ? now : c["clock"]));
    for (JsonVariantConst fixture : c["setup"].as<JsonArrayConst>()) {
      const std::string fixture_name = fixture.as<std::string>();
      ASSERT_TRUE(fixtures[fixture_name].is<JsonObjectConst>()) << "no fixture " << fixture_name;
      std::string body;
      serializeJson(fixtures[fixture_name], body);
      ASSERT_EQ(this->post("save", body).code, 200) << "fixture " << fixture_name;
    }
    if (!state.empty())
      this->put_into(state);
    this->check(c);
    if (!state.empty())
      this->take_out_of(state);
    int index = 0;
    for (JsonObjectConst then : c["then"].as<JsonArrayConst>()) {
      SCOPED_TRACE("then #" + std::to_string(++index));
      this->check(then);
    }
    ran++;
  }
  // A file that failed to load would otherwise pass, having checked nothing.
  EXPECT_GT(ran, 0u);
}

}  // namespace esphome::web_schedule_editor::testing
