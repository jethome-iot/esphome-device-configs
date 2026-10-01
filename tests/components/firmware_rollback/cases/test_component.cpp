#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>
#include "esphome/components/firmware_rollback/automation.h"
#include "esphome/components/firmware_rollback/firmware_rollback.h"
#include "esphome/components/logger/logger.h"
#include "esphome/core/base_automation.h"

namespace esphome::firmware_rollback::testing {

// The host build reads no slot, so a target is planted to see a read replace it.
class Seeded : public FirmwareRollback {
 public:
  Seeded() {
    this->target_.partition = "app1";
    this->target_.version = "2026.8.1";
    this->target_.project_name = "jxd-r6-e1eth-lcd";
  }
};

// Every config line logged since clear(). Registered once: the logger keeps its listeners.
class ConfigLog {
 public:
  std::vector<std::string> lines;

  static ConfigLog &instance() {
    static ConfigLog *log = [] {
      auto *l = new ConfigLog();
      logger::global_logger->add_log_callback(l, &ConfigLog::on_log);
      return l;
    }();
    return *log;
  }
  bool has(const char *needle) const {
    return std::any_of(this->lines.begin(), this->lines.end(),
                       [needle](const std::string &line) { return line.find(needle) != std::string::npos; });
  }

 protected:
  static void on_log(void *self, uint8_t level, const char *, const char *message, size_t len) {
    if (level == ESPHOME_LOG_LEVEL_CONFIG)
      static_cast<ConfigLog *>(self)->lines.emplace_back(message, len);
  }
};

TEST(FirmwareRollback, SetupReadsTheOtherSlot) {
  Seeded rollback;
  ASSERT_TRUE(rollback.available());
  rollback.setup();
  EXPECT_FALSE(rollback.available());
}

TEST(FirmwareRollback, RefreshFindsNothingOnTheHost) {
  Seeded rollback;
  rollback.refresh();
  EXPECT_FALSE(rollback.available());
  EXPECT_TRUE(rollback.target().partition.empty());
  EXPECT_TRUE(rollback.target().version.empty());
}

// A refused select reads the slot again, so the menu stops offering what is not there.
TEST(FirmwareRollback, AFailedRollbackSaysWhyAndReadsAgain) {
  Seeded rollback;
  EXPECT_STREQ(rollback.rollback(), "Rollback needs an ESP32");
  EXPECT_FALSE(rollback.available());
  EXPECT_STREQ(rollback.rollback(), "Rollback needs an ESP32");
  EXPECT_FALSE(rollback.available());
}

TEST(FirmwareRollback, DumpConfigNamesTheOtherSlot) {
  ConfigLog &log = ConfigLog::instance();
  Seeded rollback;
  log.lines.clear();
  rollback.dump_config();
  EXPECT_TRUE(log.has("app1, version 2026.8.1"));

  rollback.refresh();
  log.lines.clear();
  rollback.dump_config();
  EXPECT_TRUE(log.has("nothing to roll back to"));
}

TEST(RefreshAction, ReadsTheSlotAgain) {
  Seeded rollback;
  RefreshAction<> action;
  action.set_parent(&rollback);
  action.play_complex();
  EXPECT_FALSE(rollback.available());
}

TEST(IsAvailableCondition, FollowsTheLastRead) {
  Seeded rollback;
  IsAvailableCondition<> condition;
  condition.set_parent(&rollback);
  EXPECT_TRUE(condition.check());
  rollback.refresh();
  EXPECT_FALSE(condition.check());
}

// Were it to reboot, the test binary would not get past play_complex().
TEST(RollbackAction, AFailureFiresOnErrorWithTheReasonAndCarriesOn) {
  Seeded rollback;
  RollbackAction<> action;
  action.set_parent(&rollback);
  std::vector<std::string> errors;
  auto *on_error = new Automation<std::string>(action.get_error_trigger());
  on_error->add_action(new LambdaAction<std::string>([&errors, &rollback](const std::string &x) {
    // The target is read again before on_error runs.
    EXPECT_FALSE(rollback.available());
    errors.push_back(x);
  }));
  bool next_ran = false;
  ActionList<> list;
  list.add_actions({&action, new LambdaAction<>([&next_ran]() { next_ran = true; })});

  list.play();
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors[0], "Rollback needs an ESP32");
  EXPECT_TRUE(next_ran);
  EXPECT_FALSE(rollback.available());
}

TEST(RollbackAction, AFailureWithoutOnErrorIsQuiet) {
  Seeded rollback;
  RollbackAction<> action;
  action.set_parent(&rollback);
  action.play_complex();
  EXPECT_FALSE(rollback.available());
}

}  // namespace esphome::firmware_rollback::testing
