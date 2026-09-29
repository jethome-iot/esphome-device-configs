#include "common.h"

namespace esphome::crash_report::testing {

// The static listener fed lines directly, the way the logger calls it.
class Capture : public ::testing::Test {
 protected:
  void SetUp() override { this->report.start_capture(); }

  void feed(const char *tag, const std::string &line) {
    CrashReport::on_log(&this->report, ESPHOME_LOG_LEVEL_ERROR, tag, line.data(), line.size());
  }
  void feed(const std::string &line) { this->feed(CRASH_TAG, line); }

  TestCrashReport report;
};

TEST_F(Capture, KeepsTheTextAfterTheHeader) {
  const std::string text = "*** CRASH DETECTED ON PREVIOUS BOOT ***";
  const std::string line = formatted(ESPHOME_LOG_LEVEL_ERROR, CRASH_TAG, 344, nullptr, text);
  ASSERT_EQ(line.rfind("\033[1;31m[E][esp32.crash:344]: ", 0), 0u) << "the logger's header changed";
  feed(line);
  EXPECT_EQ(report.captured(), text + "\n");
}

// The thread name sits between the "]" and the ": " with a colour code of its own.
TEST_F(Capture, StripsAThreadNameBracketToo) {
  const std::string line = formatted(ESPHOME_LOG_LEVEL_ERROR, CRASH_TAG, 351, "loopTask", "  Crashed core: 1");
  ASSERT_NE(line.find("[loopTask]"), std::string::npos);
  feed(line);
  EXPECT_EQ(report.captured(), "  Crashed core: 1\n");
}

TEST_F(Capture, ReadsPlainAndHeaderlessLines) {
  const std::vector<std::pair<std::string, std::string>> cases = {
      // No colour at all.
      {"[E][esp32.crash:360]:   PC:  0x400D2A1B  (fault location)", "  PC:  0x400D2A1B  (fault location)"},
      // Level 0 has no colour code, only the reset.
      {formatted(0, CRASH_TAG, 12, nullptr, "  Reason: Abort"), "  Reason: Abort"},
      // No room left for a header: the whole line is the text.
      {"  BT0: 0x400D2A18  (backtrace)\033[0m", "  BT0: 0x400D2A18  (backtrace)"},
      // Only the first "]: " ends the header.
      {"[E][esp32.crash:1]: a]: b", "a]: b"},
      // Trailing whitespace stays as printed; an escape cut short is dropped.
      {"[E][esp32.crash:2]: text  \033[0", "text  "},
      // Longer escapes, and one in the text.
      {"\033[38;5;208m[E][esp32.crash:3]\033[0m: x\033[1my", "xy"},
      // An escape character that starts no colour code stays.
      {"[E][esp32.crash:4]: a\033b\033", "a\033b\033"},
  };
  for (const auto &[line, expected] : cases) {
    SCOPED_TRACE(line);
    report.start_capture();
    feed(line);
    EXPECT_EQ(report.captured(), expected + "\n");
  }
}

TEST_F(Capture, IgnoresOtherTags) {
  feed(formatted(ESPHOME_LOG_LEVEL_ERROR, CRASH_TAG, 1, nullptr, "one"));
  feed("esp32", formatted(ESPHOME_LOG_LEVEL_ERROR, "esp32", 1, nullptr, "not this"));
  feed("esp32.crash.x", formatted(ESPHOME_LOG_LEVEL_ERROR, "esp32.crash.x", 1, nullptr, "nor this"));
  feed(formatted(ESPHOME_LOG_LEVEL_ERROR, CRASH_TAG, 2, nullptr, "two"));
  EXPECT_EQ(report.captured(), "one\ntwo\n");
}

TEST_F(Capture, CollectsNothingOutsideACapture) {
  TestCrashReport idle;
  const std::string line = formatted(ESPHOME_LOG_LEVEL_ERROR, CRASH_TAG, 1, nullptr, "late");
  CrashReport::on_log(&idle, ESPHOME_LOG_LEVEL_ERROR, CRASH_TAG, line.data(), line.size());
  EXPECT_TRUE(idle.captured().empty());
}

}  // namespace esphome::crash_report::testing
