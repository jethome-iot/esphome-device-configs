#include "common.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include "esphome/components/logger/logger.h"

namespace esphome::automations::testing {

// A mounted directory, the way littlefs_storage presents the partition.
class FakeStorage : public filesystem_storage_abstract::FilesystemStorageAbstract {
 public:
  std::string path;
  bool mounted{true};
  bool is_mounted() const override { return this->mounted; }
  const std::string &get_base_path() const override { return this->path; }
  const char *get_filesystem_type() const override { return "Directory"; }
  // Never called here: a factory reset is the dashboard's and the menu's business.
  bool request_format() override { return false; }
};

// Every error and warning the process logs. Registered once: the logger keeps its listeners.
class LogCapture {
 public:
  std::vector<std::string> errors;
  std::vector<std::string> warnings;

  static LogCapture &instance() {
    static LogCapture *capture = [] {
      auto *c = new LogCapture();
      logger::global_logger->add_log_callback(c, &LogCapture::on_log);
      return c;
    }();
    return *capture;
  }
  void clear() {
    this->errors.clear();
    this->warnings.clear();
  }
  bool has(const std::vector<std::string> &lines, const char *needle) const {
    return std::any_of(lines.begin(), lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    auto *capture = static_cast<LogCapture *>(self);
    if (level == ESPHOME_LOG_LEVEL_ERROR) {
      capture->errors.emplace_back(message, len);
    } else if (level == ESPHOME_LOG_LEVEL_WARN) {
      capture->warnings.emplace_back(message, len);
    }
  }
};

static const char *const PRESS_RELAY_1 =
    R"({"id":1,"name":"Input press","enabled":true,"mode":"single","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})";
static const char *const ORPHAN =
    R"({"name":"Orphan","enabled":true,"mode":"single","triggers":[{"source":"input","type":"press","object_id":"no_such_input"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})";
static const char *const CRON_STEP =
    R"({"id":12,"name":"Cron Step","enabled":true,"mode":"single","triggers":[{"source":"cron","cron":"*/2 * * * * *","cron_preset":"custom"}],"actions":[{"source":"switch","type":"toggle","object_id":"relay_2"}]})";

// What the loader must refuse whole, and the error it names for each.
static const std::vector<std::pair<const char *, const char *>> REFUSED = {
    {"badaction.json",
     R"({"name":"Bad Action","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"actions":[{"source":"switch","type":"tugle","object_id":"relay_1"}]})"},
    {"bad_cond.json",
     R"({"name":"Bad Cond","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"condition":{"type":"temperature","object_id":"temp","threshold":25},"actions":[]})"},
    {"badcondtype.json",
     R"({"name":"Bad Cond Type","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"condition":{"type":"inupt","object_id":"in_2","state":"true"},"actions":[]})"},
    {"badcron.json",
     R"({"id":90,"name":"Bad Cron","triggers":[{"source":"cron","cron":"99 * * * * *","cron_preset":"custom"}],"actions":[]})"},
    {"badsource.json",
     R"({"name":"Bad Source","triggers":[{"source":"inupt","type":"press","object_id":"in_1"}],"actions":[]})"},
    {"badsub.json",
     R"({"name":"Bad Sub","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"condition":{"type":"and","conditions":[{"type":"input","object_id":"in_2"},{"type":"tempratur","object_id":"temp","temperature_type":"above","threshold":25}]},"actions":[]})"},
    {"badtype.json",
     R"({"name":"Bad Type","triggers":[{"source":"input","type":"pres","object_id":"in_1"}],"actions":[]})"},
    {"broken.json", R"({"id":91,"name":"Broken","enabled":true,"triggers":[{"sou)"},
    {"emptycond.json",
     R"({"name":"Empty Cond","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"condition":{"type":"and","conditions":[]},"actions":[]})"},
};

class Storage : public ::testing::Test {
 protected:
  void SetUp() override {
    reset_entities();
    log().clear();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->backend.path = folder;
    mkdir(this->rules().c_str(), 0755);
    this->engine = &this->new_engine();
  }

  void TearDown() override {
    for (FakeEngine *e : this->engines)
      e->forget();
    chmod(this->rules().c_str(), 0755);
    for (const std::string &name : this->files(true))
      remove((this->rules() + "/" + name).c_str());
    rmdir(this->rules().c_str());
    rmdir(this->backend.path.c_str());
  }

  // Engines outlive the test: the entities keep a callback into each one, and production never
  // destroys the component either. forget() empties them so later tests see nothing fire.
  FakeEngine &new_engine() {
    static std::vector<std::unique_ptr<FakeEngine>> all;
    all.push_back(std::make_unique<FakeEngine>());
    FakeEngine &e = *all.back();
    e.set_storage(&this->backend);
    e.with_clock();
    this->engines.push_back(&e);
    return e;
  }

  void boot() { this->engine->setup(); }
  void reboot() {
    this->engine->forget();
    this->engine = &this->new_engine();
    this->engine->setup();
  }

  std::string rules() const { return this->backend.path + "/automations"; }
  void write(const std::string &file, const std::string &json) {
    std::ofstream out(this->rules() + "/" + file);
    out << json;
  }
  std::string read(const std::string &file) const {
    std::ifstream in(this->rules() + "/" + file);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }
  // The rule files, or with `all` everything in the folder.
  std::vector<std::string> files(bool all = false) const {
    std::vector<std::string> names;
    DIR *dir = opendir(this->rules().c_str());
    if (dir == nullptr)
      return names;
    while (struct dirent *entry = readdir(dir)) {
      std::string name = entry->d_name;
      if (name == "." || name == "..")
        continue;
      if (all || (name.size() > 5 && name.substr(name.size() - 5) == ".json"))
        names.push_back(name);
    }
    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
  }
  std::vector<std::string> names() const {
    std::vector<std::string> out;
    for (const auto &config : this->engine->configs().get_all_configs())
      out.push_back(config.name);
    return out;
  }
  const AutomationConfig *config(const std::string &name) const {
    for (const auto &config : this->engine->configs().get_all_configs()) {
      if (config.name == name)
        return &config;
    }
    return nullptr;
  }
  uint32_t id_of(const std::string &name) const {
    const AutomationConfig *c = this->config(name);
    return c == nullptr ? 0 : c->id;
  }
  bool built(const std::string &name) const {
    const auto &configs = this->engine->configs().get_all_configs();
    for (size_t i = 0; i < configs.size(); i++) {
      if (configs[i].name == name)
        return this->engine->rule(i) != nullptr;
    }
    return false;
  }
  AutomationConfig rule(const char *json) {
    AutomationConfig config;
    EXPECT_TRUE(load(json, config));
    return config;
  }
  void press(binary_sensor::BinarySensor &input) {
    this->engine->dispatch_binary_sensor_(&input, true);
    this->engine->dispatch_binary_sensor_(&input, false);
  }
  static LogCapture &log() { return LogCapture::instance(); }

  FakeStorage backend;
  FakeEngine *engine{nullptr};
  std::vector<FakeEngine *> engines;
  Entities &e = entities();
};

TEST_F(Storage, RefusesToRunWithoutAMountedStorage) {
  backend.mounted = false;
  boot();
  EXPECT_TRUE(engine->is_failed());
  EXPECT_TRUE(log().has(log().errors, "Storage not mounted"));
  EXPECT_EQ(engine->add_automation(rule(PRESS_RELAY_1)), 0u);
  EXPECT_TRUE(log().has(log().errors, "Automation storage is not available"));
}

TEST_F(Storage, LoadsTheFolderAndRepairsIt) {
  write("input_press.json", PRESS_RELAY_1);
  write(
      "click.json",
      R"({"id":2,"name":"Click","mode":"single","triggers":[{"source":"input","type":"click","object_id":"in_1"}],"actions":[{"source":"switch","type":"toggle","object_id":"relay_2"}]})");
  write("zzz.json", CRON_STEP);
  write("tck.json", R"({"id":7,"name":"Tick","triggers":[{"source":"cron","cron":"* * * * * *"}],"actions":[]})");
  write(
      "longdelay.json",
      R"({"id":14,"name":"Long Delay","triggers":[{"source":"startup"}],"actions":[{"source":"delay","delay_s":5000000}]})");
  write(
      "hugedelay.json",
      R"({"id":15,"name":"Huge Delay","triggers":[{"source":"startup"}],"actions":[{"source":"delay","delay_ms":5000000000}]})");
  write(
      "disabled.json",
      R"({"id":8,"name":"Disabled","enabled":false,"triggers":[{"source":"input","type":"press","object_id":"in_1"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"}]})");
  write("noid.json", R"({"name":"No Id","triggers":[{"source":"startup"}]})");
  write("orphan.json", ORPHAN);
  for (const auto &[name, json] : REFUSED)
    write(name, json);

  boot();
  EXPECT_FALSE(engine->is_failed());
  EXPECT_EQ(engine->configs().size(), 9u);

  // Every rule sits in the file its name maps to; the refused ones are exactly as written.
  std::vector<std::string> expected = {"click.json",      "cron_step.json",   "disabled.json",
                                       "huge_delay.json", "input_press.json", "long_delay.json",
                                       "no_id.json",      "orphan.json",      "tick.json"};
  for (const auto &[name, json] : REFUSED) {
    expected.push_back(name);
    EXPECT_EQ(read(name), json) << name;
  }
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(files(), expected);

  EXPECT_NE(read("cron_step.json").find(R"("cron":"*/2 * * * * *")"), std::string::npos);
  EXPECT_NE(read("cron_step.json").find(R"("cron_preset":"custom")"), std::string::npos);
  EXPECT_EQ(read("tick.json").find("cron_preset"), std::string::npos);
  EXPECT_NE(read("long_delay.json").find(R"("delay_ms":4294967294)"), std::string::npos);
  EXPECT_NE(read("huge_delay.json").find(R"("delay_ms":4294967294)"), std::string::npos);
  EXPECT_NE(read("no_id.json").find(R"("id":)"), std::string::npos);
  EXPECT_EQ(read("orphan.json"), ORPHAN);

  EXPECT_TRUE(config("Click")->enabled);
  EXPECT_FALSE(config("Disabled")->enabled);
  EXPECT_GT(id_of("No Id"), 15u);
  EXPECT_GT(id_of("Orphan"), 15u);
  EXPECT_NE(id_of("No Id"), id_of("Orphan"));
  EXPECT_TRUE(built("No Id"));
  EXPECT_FALSE(built("Orphan"));

  // The rules that did build run; the disabled one does not.
  press(e.in1);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_EQ(e.relay2.writes, 0);

  // Each refusal names the word and the file.
  EXPECT_TRUE(log().has(log().errors, "Unknown source 'inupt'"));
  EXPECT_TRUE(log().has(log().errors, "badsource.json"));
  EXPECT_TRUE(log().has(log().errors, "Unknown type 'pres'"));
  EXPECT_TRUE(log().has(log().errors, "Unknown type 'tugle'"));
  EXPECT_TRUE(log().has(log().errors, "Unknown type 'tempratur'"));
  EXPECT_TRUE(log().has(log().errors, "Missing temperature_type"));
  EXPECT_TRUE(log().has(log().errors, "Condition 'and' has no members"));
  EXPECT_TRUE(log().has(log().errors, "Invalid cron '99 * * * * *'"));
  EXPECT_TRUE(log().has(log().errors, "JSON parse error in"));
  EXPECT_TRUE(log().has(log().errors, "Automation 'Orphan': trigger cannot be built"));
  EXPECT_TRUE(log().has(log().warnings, "Not writing 'Orphan'"));
  EXPECT_TRUE(log().has(log().warnings, "leaving 'orphan.json' in place"));

  engine->dump_config();  // lists the rule that did not build without touching it
}

TEST_F(Storage, DuplicatesGetFreshIdsAndNames) {
  write("a.json", R"({"id":5,"name":"Same","triggers":[{"source":"startup"}]})");
  write("b.json", R"({"id":5,"name":"Same","triggers":[{"source":"startup"}]})");
  boot();
  std::vector<std::string> got = names();
  std::sort(got.begin(), got.end());
  EXPECT_EQ(got, (std::vector<std::string>{"Same", "Same 2"}));
  EXPECT_EQ(id_of("Same") + id_of("Same 2"), 11u);
  EXPECT_EQ(files(), (std::vector<std::string>{"same.json", "same_2.json"}));
  EXPECT_TRUE(log().has(log().warnings, "Two automations named 'Same'"));
}

TEST_F(Storage, RulesSwappedBetweenFilesGoBackToTheirOwn) {
  write("a.json", R"({"id":1,"name":"B","triggers":[{"source":"startup"}]})");
  write("b.json", R"({"id":2,"name":"A","triggers":[{"source":"startup"}]})");
  boot();
  EXPECT_EQ(files(), (std::vector<std::string>{"a.json", "b.json"}));
  EXPECT_NE(read("a.json").find(R"("name":"A")"), std::string::npos);
  EXPECT_NE(read("b.json").find(R"("name":"B")"), std::string::npos);
  EXPECT_EQ(engine->configs().size(), 2u);
  reboot();
  EXPECT_EQ(names(), (std::vector<std::string>{"B", "A"}));  // id order: B is 1, A is 2
}

TEST_F(Storage, EmptyAndOversizedFilesAreLeftAlone) {
  write("empty.json", "");
  const std::string big = R"({"name":")" + std::string(17000, 'x') + R"(","triggers":[{"source":"startup"}]})";
  write("big.json", big);
  boot();
  EXPECT_EQ(engine->configs().size(), 0u);
  EXPECT_EQ(read("empty.json"), "");
  EXPECT_EQ(read("big.json"), big);
  EXPECT_TRUE(log().has(log().warnings, "Invalid file size"));
}

TEST_F(Storage, NamesCollideByTheFileTheyMapTo) {
  boot();
  ASSERT_NE(engine->add_automation(rule(R"({"name":"My Rule","triggers":[{"source":"startup"}]})")), 0u);
  EXPECT_TRUE(engine->is_name_taken("my_rule"));
  EXPECT_TRUE(engine->is_name_taken("  MY-RULE "));
  EXPECT_FALSE(engine->is_name_taken("Other"));
  EXPECT_EQ(engine->add_automation(rule(R"({"name":"my rule","triggers":[{"source":"startup"}]})")), 0u);
  EXPECT_TRUE(log().has(log().errors, "Refusing to add 'my rule': that name is already in use"));
  EXPECT_EQ(files(), (std::vector<std::string>{"my_rule.json"}));
}

TEST_F(Storage, FilenamesAreSanitized) {
  boot();
  std::string thirty;
  for (int i = 0; i < 30; i++)
    thirty += "ф";  // two bytes each
  for (const char *name : {"  Hello,  World!! ", "Кухня свет", "!!!"})
    ASSERT_NE(engine->add_automation(
                  rule((R"({"name":")" + std::string(name) + R"(","triggers":[{"source":"startup"}]})").c_str())),
              0u);
  ASSERT_NE(engine->add_automation(rule((R"({"name":")" + thirty + R"(","triggers":[{"source":"startup"}]})").c_str())),
            0u);
  std::string twenty_four;
  for (int i = 0; i < 24; i++)
    twenty_four += "ф";  // 48 bytes: the limit, and no character cut in half
  std::vector<std::string> expected = {"hello_world.json", "Кухня_свет.json", "automation.json", twenty_four + ".json"};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(files(), expected);
}

TEST_F(Storage, RulesRunThroughTheEntityCallback) {
  write("input_press.json", PRESS_RELAY_1);
  boot();
  e.in1.publish_state(true);
  EXPECT_TRUE(e.relay1.state);
}

TEST_F(Storage, AnAnnouncedLevelIsNotAPress) {
  write("input_press.json", PRESS_RELAY_1);
  write(
      "click.json",
      R"({"id":2,"name":"Click","triggers":[{"source":"input","type":"click","object_id":"in_1"}],"actions":[{"source":"switch","type":"toggle","object_id":"relay_2"}]})");
  boot();

  // An inversion flip re-emits the input's state: no press, and no click half-way through.
  engine->expect_level(&e.in1);
  e.in1.publish_state(true);
  EXPECT_EQ(e.relay1.writes, 0);
  engine->ms = 100;
  e.in1.publish_state(false);
  EXPECT_EQ(e.relay2.writes, 0);

  // The mark is spent: the next real edge is a press again.
  e.in1.publish_state(true);
  EXPECT_EQ(e.relay1.writes, 1);

  // An input no rule subscribed to takes no mark.
  engine->expect_level(&e.in2);
  EXPECT_TRUE(
      engine->add_automation(rule(
          R"({"name":"In 2","triggers":[{"source":"input","type":"press","object_id":"in_2"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"}]})")) !=
      0u);
  e.in2.publish_state(true);
  EXPECT_EQ(e.relay2.writes, 1);
}

TEST_F(Storage, TheApiEditsRulesAndTheirFiles) {
  boot();
  const uint32_t id = engine->add_automation(rule(
      R"({"name":"Dynamic","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})"));
  ASSERT_NE(id, 0u);
  EXPECT_EQ(files(), (std::vector<std::string>{"dynamic.json"}));
  press(e.in1);
  EXPECT_EQ(e.relay1.writes, 1);

  // A rule that cannot be built is neither kept nor written.
  AutomationConfig bad = rule(R"({"name":"Bad Cond","triggers":[{"source":"startup"}]})");
  bad.condition.type = ConditionType::TEMPERATURE;
  bad.condition.sensor_id = fnv1_hash("temp");
  EXPECT_EQ(engine->add_automation(bad), 0u);
  EXPECT_EQ(engine->configs().size(), 1u);
  EXPECT_EQ(files(), (std::vector<std::string>{"dynamic.json"}));
  EXPECT_TRUE(log().has(log().errors, "Failed to create automation 'Bad Cond'"));

  EXPECT_TRUE(engine->update_automation(
      id,
      rule(
          R"({"name":"Dynamic","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_2"}]})")));
  press(e.in1);
  EXPECT_EQ(e.relay1.writes, 1);
  EXPECT_EQ(e.relay2.writes, 1);
  EXPECT_NE(read("dynamic.json").find("relay_2"), std::string::npos);

  // A rename moves the file; a name in use is refused.
  EXPECT_TRUE(engine->update_automation(id, rule(R"({"name":"Renamed","triggers":[{"source":"startup"}]})")));
  EXPECT_EQ(files(), (std::vector<std::string>{"renamed.json"}));
  EXPECT_EQ(id_of("Renamed"), id);
  const uint32_t second = engine->add_automation(rule(R"({"name":"Second","triggers":[{"source":"startup"}]})"));
  ASSERT_NE(second, 0u);
  EXPECT_NE(second, id);
  EXPECT_FALSE(engine->update_automation(second, rule(R"({"name":"renamed","triggers":[{"source":"startup"}]})")));
  EXPECT_TRUE(log().has(log().errors, "Refusing to rename automation"));
  EXPECT_FALSE(engine->update_automation(99, rule(R"({"name":"Nobody","triggers":[{"source":"startup"}]})")));
  EXPECT_FALSE(engine->remove_automation(99));

  EXPECT_TRUE(engine->remove_automation(id));
  EXPECT_EQ(files(), (std::vector<std::string>{"second.json"}));
  EXPECT_EQ(engine->configs().size(), 1u);
}

TEST_F(Storage, DisablingIsPersistedUnlessTheRuleIsAnOrphan) {
  write(
      "delayed.json",
      R"({"id":10,"name":"Delayed","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"},{"source":"delay","delay_ms":900},{"source":"switch","type":"turn_on","object_id":"relay_2"}]})");
  write("orphan.json", ORPHAN);
  boot();

  press(e.in1);
  EXPECT_TRUE(e.relay1.state);
  ASSERT_EQ(engine->delays.size(), 1u);
  bool persisted = false;
  EXPECT_TRUE(engine->set_enable_automation(10, false, &persisted));
  EXPECT_TRUE(persisted);
  EXPECT_TRUE(engine->delays.empty());  // the pending step went with the rule
  EXPECT_NE(read("delayed.json").find(R"("enabled":false)"), std::string::npos);

  // The orphan takes the change but its file is the only record of the input it names.
  persisted = true;
  EXPECT_TRUE(engine->set_enable_automation(id_of("Orphan"), false, &persisted));
  EXPECT_FALSE(persisted);
  EXPECT_EQ(read("orphan.json"), ORPHAN);
  EXPECT_TRUE(log().has(log().warnings, "disabled, but the change was not saved"));
  EXPECT_FALSE(engine->set_enable_automation(99, false, &persisted));

  reboot();
  EXPECT_FALSE(config("Delayed")->enabled);
  EXPECT_TRUE(config("Orphan")->enabled);
  reset_entities();
  press(e.in1);
  EXPECT_EQ(e.relay1.writes, 0);
}

TEST_F(Storage, ASecondBootReadsBackTheFolder) {
  write("input_press.json", PRESS_RELAY_1);
  write("zzz.json", CRON_STEP);
  write("orphan.json", ORPHAN);
  boot();
  EXPECT_EQ(id_of("Orphan"), 13u);
  EXPECT_EQ(engine->add_automation(rule(R"({"name":"Dynamic","triggers":[{"source":"startup"}]})")), 14u);

  reboot();
  // In id order; the orphan is restamped on every boot because its file is never rewritten.
  EXPECT_EQ(names(), (std::vector<std::string>{"Input press", "Cron Step", "Dynamic", "Orphan"}));
  EXPECT_EQ(id_of("Orphan"), 15u);
  EXPECT_EQ(read("orphan.json"), ORPHAN);

  engine->reset_all();
  EXPECT_EQ(engine->configs().size(), 0u);
  EXPECT_TRUE(files().empty());
}

TEST_F(Storage, ARefusedFileIsNeverWrittenOver) {
  const char *refused =
      R"({"name":"Porch","triggers":[{"source":"input","type":"pres","object_id":"in_1"}],"actions":[]})";
  write("porch.json", refused);
  write("x.json", R"({"id":1,"name":"Porch","triggers":[{"source":"startup"}]})");
  boot();
  EXPECT_EQ(engine->configs().size(), 1u);
  EXPECT_EQ(read("porch.json"), refused);
  EXPECT_NE(read("x.json").find(R"("name":"Porch")"), std::string::npos);
  EXPECT_TRUE(log().has(log().warnings, "Not writing 'porch.json': a file the loader refused is there"));

  // The API is held to the same rule, for a new name and for a rename.
  EXPECT_TRUE(engine->remove_automation(1));
  EXPECT_EQ(engine->add_automation(rule(R"({"name":"porch","triggers":[{"source":"startup"}]})")), 0u);
  EXPECT_TRUE(log().has(log().errors, "Refusing to add 'porch': a file the loader refused has that name"));
  const uint32_t other = engine->add_automation(rule(R"({"name":"Other","triggers":[{"source":"startup"}]})"));
  ASSERT_NE(other, 0u);
  EXPECT_FALSE(engine->update_automation(other, rule(R"({"name":"Porch","triggers":[{"source":"startup"}]})")));
  EXPECT_EQ(read("porch.json"), refused);
  EXPECT_EQ(files(), (std::vector<std::string>{"other.json", "porch.json"}));
}

TEST_F(Storage, SavesLeaveNoHalfWrittenFile) {
  boot();
  ASSERT_NE(engine->add_automation(rule(R"({"name":"Whole","triggers":[{"source":"startup"}]})")), 0u);
  EXPECT_EQ(files(true), (std::vector<std::string>{"whole.json"}));
}

TEST_F(Storage, AnUnwritableFolderKeepsTheRuleLiveButUnsaved) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes anywhere";
  boot();
  chmod(rules().c_str(), 0500);
  const uint32_t id = engine->add_automation(rule(
      R"({"name":"Volatile","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})"));
  ASSERT_NE(id, 0u);
  EXPECT_TRUE(log().has(log().warnings, "created but not saved"));
  press(e.in1);
  EXPECT_TRUE(e.relay1.state);
  chmod(rules().c_str(), 0755);
  EXPECT_TRUE(files(true).empty());
  reboot();
  EXPECT_EQ(engine->configs().size(), 0u);
}

TEST_F(Storage, RefusesToRunOverAFolderItCannotRead) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root reads anywhere";
  chmod(rules().c_str(), 0000);
  boot();
  EXPECT_TRUE(engine->is_failed());
  EXPECT_TRUE(log().has(log().errors, "Cannot read"));
}

TEST_F(Storage, IdsWrapBeforeTheTimerRange) {
  write("top.json", R"({"id":268435455,"name":"Top","triggers":[{"source":"startup"}]})");
  write("noid.json", R"({"name":"No Id","triggers":[{"source":"startup"}]})");
  boot();
  EXPECT_EQ(id_of("Top"), 268435455u);
  EXPECT_EQ(id_of("No Id"), 1u);
  EXPECT_EQ(engine->add_automation(rule(R"({"name":"Next","triggers":[{"source":"startup"}]})")), 2u);
}

TEST_F(Storage, RulesCannotBeEditedFromADelayedStepEither) {
  write(
      "delayed.json",
      R"({"id":1,"name":"Delayed","triggers":[{"source":"input","type":"press","object_id":"in_1"}],"actions":[{"source":"delay","delay_ms":500},{"source":"switch","type":"turn_on","object_id":"relay_1"}]})");
  boot();
  bool removed = true;
  e.relay1.on_change = [&]() { removed = engine->remove_automation(1); };
  press(e.in1);
  ASSERT_EQ(engine->delays.size(), 1u);
  engine->fire_next();
  EXPECT_TRUE(e.relay1.state);
  EXPECT_FALSE(removed);
  EXPECT_EQ(engine->configs().size(), 1u);
}

TEST_F(Storage, AnIdTooLargeForATimerIsRestamped) {
  write("big.json", R"({"id":300000000,"name":"Big","triggers":[{"source":"startup"}]})");
  boot();
  EXPECT_EQ(id_of("Big"), 1u);
  EXPECT_NE(read("big.json").find(R"("id":1,)"), std::string::npos);
}

TEST_F(Storage, StartupRulesFireOnceTheEngineIsUp) {
  write(
      "boot.json",
      R"({"id":1,"name":"Boot","triggers":[{"source":"startup"}],"actions":[{"source":"switch","type":"turn_on","object_id":"relay_1"}]})");
  boot();
  engine->rule(0)->on_startup();  // what setup() defers until every component is up
  EXPECT_TRUE(e.relay1.state);
}

TEST_F(Storage, RulesCannotBeEditedFromInsideTheirOwnAction) {
  write("input_press.json", PRESS_RELAY_1);
  boot();
  bool removed = true;
  e.relay1.on_change = [&]() { removed = engine->remove_automation(1); };
  press(e.in1);
  EXPECT_TRUE(e.relay1.state);
  EXPECT_FALSE(removed);
  EXPECT_EQ(engine->configs().size(), 1u);
  EXPECT_TRUE(log().has(log().errors, "Rules cannot be edited from inside a rule's own action"));
  // Free again once the dispatch has returned.
  EXPECT_TRUE(engine->remove_automation(1));
}

TEST_F(Storage, RefusesARuleThatWouldNotFitItsFile) {
  boot();
  // Each action is about 60 bytes on disk; 400 of them pass the 16 KiB the loader takes.
  std::string json = R"({"name":"Huge","triggers":[{"source":"startup"}],"actions":[)";
  for (int i = 0; i < 400; i++)
    json += (i ? "," : "") + std::string(R"({"source":"switch","type":"toggle","object_id":"relay_1"})");
  json += "]}";
  EXPECT_EQ(engine->add_automation(rule(json.c_str())), 0u);
  EXPECT_TRUE(log().has(log().errors, "does not fit a 16384 byte file"));
  EXPECT_TRUE(files().empty());
  const uint32_t id = engine->add_automation(rule(R"({"name":"Small","triggers":[{"source":"startup"}]})"));
  ASSERT_NE(id, 0u);
  AutomationConfig huge = rule(json.c_str());
  huge.name = "Small";
  EXPECT_FALSE(engine->update_automation(id, huge));
  EXPECT_EQ(engine->configs().get_all_configs()[0].actions.size(), 0u);
}

TEST_F(Storage, StopsAt255Rules) {
  boot();
  for (int i = 1; i <= 255; i++)
    ASSERT_NE(engine->add_automation(
                  rule((R"({"name":"Rule )" + std::to_string(i) + R"(","triggers":[{"source":"startup"}]})").c_str())),
              0u);
  EXPECT_EQ(engine->add_automation(rule(R"({"name":"One more","triggers":[{"source":"startup"}]})")), 0u);
  EXPECT_TRUE(log().has(log().errors, "already at the 255 automation limit"));
  EXPECT_EQ(files().size(), 255u);
}

}  // namespace esphome::automations::testing
