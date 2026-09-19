#include <gtest/gtest.h>
#include "esphome/components/loop_job/loop_job.h"
#include "esphome/core/component.h"

namespace esphome::loop_job::testing {

// The slot is the whole of what the two tasks share, so the cases step it by hand: a real
// race is not reproducible, but every order the two sides can reach it in is.

TEST(Slot, RunsTheJobAndKeepsItsAnswer) {
  int ran = 0;
  LoopJobSlot slot([&]() {
    ran++;
    return true;
  });
  EXPECT_FALSE(slot.done());

  slot.run();
  EXPECT_EQ(ran, 1);
  EXPECT_TRUE(slot.done());
  EXPECT_TRUE(slot.result());
}

TEST(Slot, AFalseAnswerComesBackAsFalse) {
  LoopJobSlot slot([]() { return false; });
  slot.run();
  EXPECT_TRUE(slot.done());
  EXPECT_FALSE(slot.result());
}

// The caller gave up first: the job must not touch the state it was still waiting on, because
// the caller has already answered for it and the references it captured are going out of scope.
TEST(Slot, AnAbandonedJobNeverRuns) {
  int ran = 0;
  LoopJobSlot slot([&]() {
    ran++;
    return true;
  });

  EXPECT_TRUE(slot.abandon());
  slot.run();
  EXPECT_EQ(ran, 0);
  EXPECT_FALSE(slot.done());
}

// The other side of the same race: the loop started at the deadline, so the caller loses the
// slot and has to wait rather than report a timeout over a job that is changing the device.
TEST(Slot, AJobThatStartedCannotBeAbandoned) {
  LoopJobSlot slot([]() { return true; });
  slot.run();
  EXPECT_FALSE(slot.abandon());
  EXPECT_TRUE(slot.done());
  EXPECT_TRUE(slot.result());
}

// Abandoning from inside the job is what a caller hitting the deadline mid-run does.
TEST(Slot, AbandonDuringTheJobLosesToIt) {
  LoopJobSlot *self = nullptr;
  bool abandoned = true;
  LoopJobSlot slot([&]() {
    abandoned = self->abandon();
    return true;
  });
  self = &slot;

  slot.run();
  EXPECT_FALSE(abandoned);
  EXPECT_TRUE(slot.done());
}

TEST(Slot, TheJobRunsOnce) {
  int ran = 0;
  LoopJobSlot slot([&]() {
    ran++;
    return true;
  });
  slot.run();
  slot.run();
  EXPECT_EQ(ran, 1);
}

TEST(Slot, AbandonIsFinal) {
  LoopJobSlot slot([]() { return true; });
  EXPECT_TRUE(slot.abandon());
  EXPECT_FALSE(slot.abandon());
}

class Owner : public Component {};

// On a build with one task every caller is the loop task, so the job runs inline and the
// dispatcher is a straight call. The ESP32 path needs two tasks and lives in QEMU.
TEST(Dispatcher, RunsInlineWhenTheCallerIsTheLoopTask) {
  Owner owner;
  LoopDispatcher dispatcher;
  dispatcher.capture_loop_task();

  int ran = 0;
  EXPECT_TRUE(dispatcher.run_on_loop(&owner, [&]() {
    ran++;
    return true;
  }));
  EXPECT_EQ(ran, 1);
  EXPECT_FALSE(dispatcher.run_on_loop(&owner, []() { return false; }));
}

}  // namespace esphome::loop_job::testing
