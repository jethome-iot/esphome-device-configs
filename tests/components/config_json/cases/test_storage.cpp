#include "common.h"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include "esphome/core/application.h"
#include "esphome/core/hal.h"

namespace esphome::config_json::testing {

static const char *const VALID =
    R"({"version":1,"records":[{"source_name":"sw_a","inverted":true,"level":7},{"level":3},{"source_name":"sw_b","inverted":false,"level":2}]})";

// The keeper over a temporary directory, the way the firmware runs it over the partition.
class Storage : public ::testing::Test {
 protected:
  void SetUp() override {
    log().clear();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->backend.path = folder;
    this->keeper = &this->new_keeper();
  }

  void TearDown() override {
    // A timer left behind would fire into this fixture's settings from a later test.
    for (ConfigJsonKeeper *k : this->keepers)
      k->cancel_pending_save();
    chmod(this->dir().c_str(), 0755);
    for (const std::string &name : this->files())
      remove((this->dir() + "/" + name).c_str());
    rmdir(this->dir().c_str());
    rmdir(this->backend.path.c_str());
  }

  // Keepers outlive the test: a pending save is a scheduler entry pointing at one, and the
  // scheduler never runs here, so nothing ever fires on a stale pointer either way.
  ConfigJsonKeeper &new_keeper() {
    static std::vector<std::unique_ptr<ConfigJsonKeeper>> all;
    all.push_back(std::make_unique<ConfigJsonKeeper>());
    ConfigJsonKeeper &k = *all.back();
    k.set_storage(&this->backend);
    k.set_config_dir("config");
    k.set_save_delay(400);
    this->keepers.push_back(&k);
    return k;
  }

  // Lets the wall clock advance and runs what the scheduler has due: the only way a timeout
  // fires in this harness.
  static void pass(uint32_t ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    App.scheduler.call(millis());
  }

  void boot() {
    this->keeper->add_settings(&this->settings);
    this->keeper->setup();
  }
  // A second boot reads what the first one wrote.
  TestSettings &reboot() {
    this->keeper = &this->new_keeper();
    this->keeper->add_settings(&this->second);
    this->keeper->setup();
    return this->second;
  }

  std::string dir() const { return this->backend.path + "/config"; }
  std::string file() const { return this->dir() + "/test.json"; }
  void write(const std::string &text) {
    mkdir(this->dir().c_str(), 0755);
    std::ofstream out(this->file());
    out << text;
  }
  std::string read() const {
    std::ifstream in(this->file());
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
  }
  long size() const {
    struct stat st {};
    return stat(this->file().c_str(), &st) == 0 ? st.st_size : -1;
  }
  std::vector<std::string> files() const {
    std::vector<std::string> names;
    DIR *d = opendir(this->dir().c_str());
    if (d == nullptr)
      return names;
    while (struct dirent *entry = readdir(d)) {
      std::string name = entry->d_name;
      if (name != "." && name != "..")
        names.push_back(name);
    }
    closedir(d);
    std::sort(names.begin(), names.end());
    return names;
  }
  static LogCapture &log() { return LogCapture::instance(); }

  FakeStorage backend;
  ConfigJsonKeeper *keeper{nullptr};
  std::vector<ConfigJsonKeeper *> keepers;
  TestSettings settings;
  TestSettings second;
};

TEST_F(Storage, RefusesToRunWithoutAMountedStorage) {
  backend.mounted = false;
  boot();
  EXPECT_TRUE(keeper->is_failed());
  EXPECT_TRUE(log().has(log().errors, "not mounted"));
}

TEST_F(Storage, CreatesTheDirectoryAndStartsEmpty) {
  boot();
  EXPECT_FALSE(keeper->is_failed());
  EXPECT_TRUE(files().empty());
  EXPECT_EQ(settings.size(), 0u);
  EXPECT_FALSE(settings.is_dirty());
  // Nothing dirty: a boot writes nothing.
  keeper->save_immediate();
  EXPECT_EQ(size(), -1);
}

TEST_F(Storage, LoadsTheRecordsAndDropsOnlyTheBadOne) {
  write(VALID);
  boot();
  EXPECT_EQ(settings.report(), "sw_a=1/7 sw_b=0/2");
  EXPECT_TRUE(log().has(log().warnings, "Failed to parse record"));
  EXPECT_FALSE(settings.is_dirty());
  settings.apply();
  EXPECT_EQ(settings.applied, (std::vector<std::string>{"sw_a", "sw_b"}));
}

TEST_F(Storage, TheLastOfTwoRecordsForOneEntityWins) {
  write(R"({"version":1,"records":[{"source_name":"sw_a","level":1},{"source_name":"sw_b","level":2},)"
        R"({"source_name":"sw_a","inverted":true,"level":3}]})");
  boot();
  EXPECT_EQ(settings.report(), "sw_a=1/3 sw_b=0/2");  // in the first one's place
  EXPECT_TRUE(log().has(log().warnings, "Duplicate record for 'sw_a'"));
  settings.apply();
  EXPECT_EQ(settings.applied, (std::vector<std::string>{"sw_a", "sw_b"}));
}

TEST_F(Storage, LeavesADamagedFileAloneAndStartsFromDefaults) {
  struct Case {
    const char *name;
    std::string text;
    const char *message;
    bool warning;
  };
  const std::vector<Case> cases = {
      {"corrupt", R"({"version":1,"records":[{"source_name":"sw_a","inverted":true,"lev)", "Failed to parse JSON",
       false},
      {"empty", "", "Invalid file size", true},
      {"not an object", "[1,2,3]", "Failed to parse JSON", false},
      {"records not an array", R"({"version":1,"records":{"a":1}})", "Failed to parse test settings", false},
      {"too large", std::string(70000, ' '), "Invalid file size", true},
  };
  for (const Case &c : cases) {
    SCOPED_TRACE(c.name);
    log().clear();
    write(c.text);
    const long before = size();
    TestSettings fresh;
    ConfigJsonKeeper &k = new_keeper();
    k.add_settings(&fresh);
    k.setup();
    EXPECT_EQ(fresh.size(), 0u);
    EXPECT_FALSE(fresh.is_dirty());
    EXPECT_TRUE(log().has(c.warning ? log().warnings : log().errors, c.message));
    // Not dirty, so neither a save nor the shutdown flush rewrites it.
    k.save_immediate();
    k.on_shutdown();
    EXPECT_EQ(size(), before);
    EXPECT_EQ(read(), c.text);
  }
}

TEST_F(Storage, ASecondEditRestartsTheDelayAndOneWriteFollows) {
  boot();
  settings.update("sw_b", true, 42);
  keeper->save();
  pass(200);
  settings.update("sw_a", false, 7);
  keeper->save();  // the 400 ms start over
  pass(200);       // the first timer would have fired by now
  EXPECT_TRUE(keeper->is_save_pending());
  EXPECT_EQ(size(), -1);

  pass(300);  // past the second one
  EXPECT_FALSE(keeper->is_save_pending());
  EXPECT_FALSE(settings.is_dirty());
  EXPECT_EQ(files(), (std::vector<std::string>{"test.json"}));  // the .tmp is gone
  EXPECT_EQ(read(), R"({"version":1,"records":[{"source_name":"sw_b","inverted":true,"level":42},)"
                    R"({"source_name":"sw_a","inverted":false,"level":7}]})");

  EXPECT_EQ(reboot().report(), "sw_b=1/42 sw_a=0/7");
}

TEST_F(Storage, SaveImmediateWritesNowAndDropsThePendingTimer) {
  boot();
  settings.update("sw_a", true, 1);
  keeper->save();
  keeper->save_immediate();
  EXPECT_FALSE(keeper->is_save_pending());
  EXPECT_GT(size(), 0);
  const long written = size();
  settings.update("sw_a", false, 2);  // dirty again, but nothing scheduled
  pass(500);
  EXPECT_EQ(size(), written);
  EXPECT_TRUE(settings.is_dirty());
}

TEST_F(Storage, ASaveByKeyOnlyWritesThatType) {
  boot();
  settings.update("sw_a", true, 1);
  keeper->save("nope");
  EXPECT_TRUE(log().has(log().warnings, "Settings 'nope' not found"));
  EXPECT_FALSE(keeper->is_save_pending());
  keeper->save_immediate("nope");
  EXPECT_EQ(size(), -1);
  keeper->save_immediate("test");
  EXPECT_GT(size(), 0);
  EXPECT_FALSE(settings.is_dirty());
}

TEST_F(Storage, ShutdownFlushesATypeWhoseSaveFailed) {
  boot();
  settings.update("sw_a", true, 1);
  ASSERT_EQ(chmod(dir().c_str(), 0555), 0);
  keeper->save_immediate();
  EXPECT_TRUE(log().has(log().errors, "Failed to open"));
  EXPECT_TRUE(settings.is_dirty());
  EXPECT_FALSE(keeper->is_save_pending());
  EXPECT_TRUE(files().empty());  // no half-written file either

  chmod(dir().c_str(), 0755);
  keeper->on_shutdown();
  EXPECT_FALSE(settings.is_dirty());
  EXPECT_EQ(reboot().report(), "sw_a=1/1");
}

TEST_F(Storage, ATypeTooLargeForTheLoaderIsNotWritten) {
  write(VALID);
  boot();
  const long before = size();
  // ~90 bytes a record: 800 of them pass the 64 KiB the loader accepts.
  for (int i = 0; i < 800; i++)
    settings.update("record_with_a_long_name_to_fill_the_file_" + std::to_string(i), true, i);
  keeper->save_immediate();
  EXPECT_TRUE(log().has(log().errors, "over the 65536 byte limit"));
  EXPECT_TRUE(settings.is_dirty());
  EXPECT_EQ(size(), before);  // the last good file stays
  EXPECT_EQ(files(), (std::vector<std::string>{"test.json"}));
}

TEST_F(Storage, ResetAllClearsTheRecordsAndWritesAtOnce) {
  write(VALID);
  boot();
  keeper->reset_all();
  EXPECT_EQ(settings.size(), 0u);
  EXPECT_EQ(read(), R"({"version":1,"records":[]})");
}

}  // namespace esphome::config_json::testing
