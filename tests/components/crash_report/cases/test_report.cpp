#include "common.h"
#include <sys/resource.h>
#include <csignal>
#include <cstdio>
#include <regex>
#include "esphome/core/application.h"

namespace esphome::crash_report::testing {

// A worst-case record for a dual-core chip, line for line as crash_handler_log() prints it:
// 16 frames a core and both addr2line hints filled to their buffer.
static std::vector<std::string> dual_core_record() {
  std::vector<std::string> lines = {
      "*** CRASH DETECTED ON PREVIOUS BOOT ***",
      "  Reason: Fault - InstrFetchProhibited (cause 20)",
      "  Crashed core: 1",
      "  PC:  0x400D2A1B  (fault location)",
      "  EXCVADDR: 0x00000010  (faulting address)",
  };
  char text[256];
  for (unsigned i = 0; i < 16; i++) {
    snprintf(text, sizeof(text), "  BT%u: 0x%08X  (backtrace)", i, 0x400D2A18 + 4 * i);
    lines.emplace_back(text);
  }
  lines.emplace_back("  Other core (0) backtrace:");
  for (unsigned i = 0; i < 16; i++) {
    snprintf(text, sizeof(text), "  BT%u: 0x%08X  (backtrace)", i, 0x40081234 + 4 * i);
    lines.emplace_back(text);
  }
  const std::pair<const char *, unsigned> hints[] = {
      {"Use: addr2line -pfiaC -e firmware.elf 0x400D2A1B", 0x400D2A18},
      {"Other core: addr2line -pfiaC -e firmware.elf", 0x40081234},
  };
  for (const auto &[prefix, base] : hints) {
    int pos = snprintf(text, sizeof(text), "%s", prefix);
    for (unsigned i = 0; i < 16 && pos < static_cast<int>(sizeof(text)) - 12; i++)
      pos += snprintf(text + pos, sizeof(text) - pos, " 0x%08X", base + 4 * i);
    lines.emplace_back(text);
  }
  return lines;
}

static std::string joined(const std::vector<std::string> &lines) {
  std::string out;
  for (const std::string &line : lines)
    out += line + "\n";
  return out;
}

// The report over a temporary directory, the way the firmware runs it over the partition.
class Report : public ::testing::Test {
 protected:
  void SetUp() override {
    log().clear();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->backend.path = folder;
    TestCrashReport &r = report();
    r.record = false;
    r.lines.clear();
    r.clears = 0;
    r.set_storage(&this->backend);
    r.set_report_dir("crash");
    r.set_keep(4);
  }

  void TearDown() override {
    chmod(this->backend.path.c_str(), 0755);
    chmod(this->dir().c_str(), 0755);
    std::filesystem::remove_all(this->backend.path);
  }

  static TestCrashReport &report() { return shared_report(); }
  static LogCapture &log() { return LogCapture::instance(); }

  // A panic record that setup() finds, with an interleaved line from another tag.
  void crash(const std::vector<std::string> &lines) {
    TestCrashReport &r = report();
    r.record = true;
    r.lines.clear();
    for (size_t i = 0; i < lines.size(); i++) {
      r.lines.emplace_back(CRASH_TAG, lines[i]);
      if (i == 1)
        r.lines.emplace_back("wifi", "not part of the record");
    }
  }
  void crash(const std::string &pc) {
    this->crash({"*** CRASH DETECTED ON PREVIOUS BOOT ***", "  Reason: Abort", "  Crashed core: 0",
                 "  PC:  " + pc + "  (fault location)"});
  }

  std::string dir(const char *name = "crash") const { return this->backend.path + "/" + name; }
  std::string file(int n, const char *name = "crash") const {
    return this->dir(name) + "/crash" + std::to_string(n) + ".txt";
  }
  std::vector<std::string> files(const char *name = "crash") const {
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(this->dir(name), ec))
      names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    return names;
  }
  static std::string header() { return report().header(); }

  FakeStorage backend;
};

TEST_F(Report, NoRecordWritesNothingAndClearsNothing) {
  report().setup();
  EXPECT_FALSE(std::filesystem::exists(dir()));
  EXPECT_EQ(report().clears, 0);
}

TEST_F(Report, TheHeaderNamesTheFirmwareThatWroteIt) {
  const std::string text = header();
  std::vector<std::string> lines;
  std::stringstream in(text);
  for (std::string line; std::getline(in, line);)
    lines.push_back(line);
  ASSERT_EQ(lines.size(), 2u) << text;  // no elf_sha256 off ESP32
  EXPECT_EQ(lines[0], "firmware: jethome.crash_report_test 1.2.3");
  char build_time[Application::BUILD_TIME_STR_SIZE];
  App.get_build_time_string(build_time);
  EXPECT_EQ(lines[1], std::string("build: ") + build_time);
  EXPECT_TRUE(std::regex_match(lines[1], std::regex(R"(build: \d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2} [+-]\d{4})")))
      << lines[1];
  EXPECT_EQ(text.find("elf_sha256"), std::string::npos);
}

TEST_F(Report, WritesTheHeaderThenTheRecordAndClearsIt) {
  crash("0x400D2A1B");
  report().setup();
  EXPECT_EQ(files(), (std::vector<std::string>{"crash0.txt"}));
  EXPECT_EQ(read_file(file(0)), header() + "*** CRASH DETECTED ON PREVIOUS BOOT ***\n"
                                           "  Reason: Abort\n"
                                           "  Crashed core: 0\n"
                                           "  PC:  0x400D2A1B  (fault location)\n");
  EXPECT_EQ(report().clears, 1);
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_WARN, "Crash report saved to"));
}

TEST_F(Report, KeepsAWorstCaseDualCoreRecordWhole) {
  const std::vector<std::string> record = dual_core_record();
  crash(record);
  report().setup();
  EXPECT_EQ(read_file(file(0)), header() + joined(record));
  EXPECT_EQ(report().clears, 1);
}

// Cut at a line, never inside one, and nothing after the first line that did not fit.
TEST_F(Report, CutsALongerRecordAtFourKilobytes) {
  std::vector<std::string> record;
  for (int i = 0; i < 200; i++)
    record.push_back("  bt" + std::to_string(i) + ": 0x400d2a18 " + std::string(20, '.'));
  record.emplace_back("x");  // would still fit after the cut
  crash(record);
  report().setup();

  const std::string text = read_file(file(0));
  EXPECT_LE(text.size(), 4096u);
  ASSERT_EQ(text.rfind(header(), 0), 0u);
  std::string expected = header();
  size_t kept = 0;
  while (expected.size() + record[kept].size() + 1 <= 4096)
    expected += record[kept++] + "\n";
  EXPECT_LT(kept, 200u);
  EXPECT_EQ(text, expected);
  EXPECT_EQ(report().clears, 1);
}

TEST_F(Report, RotatesAndDropsTheOldest) {
  for (int keep : {1, 2, 4}) {
    SCOPED_TRACE(keep);
    const std::string name = "keep" + std::to_string(keep);
    report().set_report_dir(name);
    report().set_keep(keep);
    const int written = keep + 2;
    for (int n = 1; n <= written; n++) {
      char pc[16];
      snprintf(pc, sizeof(pc), "0x%08X", n);
      crash(pc);
      report().setup();
    }
    std::vector<std::string> expected;
    for (int i = 0; i < keep; i++)
      expected.push_back("crash" + std::to_string(i) + ".txt");
    EXPECT_EQ(files(name.c_str()), expected);
    // Newest first: crash0 holds the last report written.
    for (int i = 0; i < keep; i++) {
      char pc[16];
      snprintf(pc, sizeof(pc), "0x%08X", written - i);
      EXPECT_NE(read_file(file(i, name.c_str())).find(pc), std::string::npos) << "crash" << i;
    }
    EXPECT_EQ(report().clears, written) << "one clear per report";
    report().clears = 0;
  }
}

TEST_F(Report, AnUnmountedStorageKeepsTheRecord) {
  crash("0x400D2A1B");
  backend.mounted = false;
  report().setup();
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_WARN, "storage is not mounted"));
  EXPECT_FALSE(std::filesystem::exists(dir()));
  EXPECT_EQ(report().clears, 0);

  report().set_storage(nullptr);
  report().setup();
  EXPECT_EQ(report().clears, 0);
}

TEST_F(Report, AFolderThatCannotBeCreatedKeepsTheRecord) {
  crash("0x400D2A1B");
  ASSERT_EQ(chmod(backend.path.c_str(), 0555), 0);
  report().setup();
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_ERROR, "Cannot create"));
  EXPECT_FALSE(std::filesystem::exists(dir()));
  EXPECT_EQ(report().clears, 0);
}

TEST_F(Report, AFileThatCannotBeOpenedKeepsTheRecordForTheNextBoot) {
  crash("0x400D2A1B");
  ASSERT_EQ(mkdir(dir().c_str(), 0755), 0);
  ASSERT_EQ(chmod(dir().c_str(), 0555), 0);
  report().setup();
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_ERROR, "Cannot open"));
  EXPECT_TRUE(files().empty());
  EXPECT_EQ(report().clears, 0);

  // The next boot finds the record still there and gets it on disk.
  chmod(dir().c_str(), 0755);
  report().setup();
  EXPECT_EQ(files(), (std::vector<std::string>{"crash0.txt"}));
  EXPECT_EQ(report().clears, 1);
}

// A full filesystem shows up at close; the size limit stands in for it on the host.
TEST_F(Report, AWriteThatFailsAtCloseLeavesNoFileAndKeepsTheRecord) {
  crash("0x400D2A1B");
  struct rlimit saved {};
  ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &saved), 0);
  auto *previous = signal(SIGXFSZ, SIG_IGN);
  struct rlimit tiny = saved;
  tiny.rlim_cur = 16;
  ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &tiny), 0);
  report().setup();
  setrlimit(RLIMIT_FSIZE, &saved);
  signal(SIGXFSZ, previous);

  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_ERROR, "Failed to write"));
  EXPECT_TRUE(files().empty()) << "the partial file stayed";
  EXPECT_EQ(report().clears, 0);
}

TEST_F(Report, ARecordWithNoLinesWritesNothing) {
  report().record = true;
  report().lines = {{"wifi", "not part of the record"}};
  report().setup();
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_WARN, "reported no lines"));
  EXPECT_FALSE(std::filesystem::exists(dir()));
  EXPECT_EQ(report().clears, 0);
}

// Without ESPHome's crash handler (off ESP32, or with Arduino linked) there is never a record.
TEST_F(Report, WithoutTheCrashHandlerThereIsNoRecord) {
  struct Plain : CrashReport {
    using CrashReport::clear_record_;
    using CrashReport::emit_record_;
    using CrashReport::has_record_;
  } plain;
  plain.set_storage(&backend);
  plain.set_report_dir("crash");
  plain.setup();
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_DEBUG, "No crash record"));
  EXPECT_FALSE(std::filesystem::exists(dir()));

  EXPECT_FALSE(plain.has_record_());
  log().clear();
  plain.emit_record_();
  plain.clear_record_();
  EXPECT_TRUE(log().lines.empty());
}

TEST_F(Report, SetsUpRightAfterTheMountAndBeforeTheSettings) {
  const float priority = report().get_setup_priority();
  EXPECT_LT(priority, setup_priority::HARDWARE + 10.0f);  // littlefs_storage's mount
  EXPECT_GT(priority, setup_priority::HARDWARE + 5.0f);   // config_json's keeper
}

TEST_F(Report, DumpConfigNamesTheFolder) {
  report().dump_config();
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_CONFIG, dir().c_str()));
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_CONFIG, "Keep: 4"));
  log().clear();
  backend.mounted = false;
  report().dump_config();
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_CONFIG, "Storage not mounted"));
  log().clear();
  report().set_storage(nullptr);
  report().dump_config();
  EXPECT_TRUE(log().has(ESPHOME_LOG_LEVEL_CONFIG, "Storage not mounted"));
}

}  // namespace esphome::crash_report::testing
