// The timing alone: what a virtual input shows from its source's level and the clock.
#include "common.h"

namespace esphome::virtual_inputs::testing {

static DelayLine line(uint32_t on_ms, uint32_t off_ms) {
  DelayLine l;
  l.set_delays(on_ms, off_ms);
  return l;
}

TEST(DelayLine, StartsOffWithNothingHeard) {
  DelayLine l = line(1000, 1000);
  EXPECT_FALSE(l.output());
  EXPECT_FALSE(l.has_level());
  EXPECT_FALSE(l.pending());
  EXPECT_FALSE(l.tick(5000));
}

TEST(DelayLine, TurnsOnOnceTheSourceHasStayedOnForTheOnDelay) {
  DelayLine l = line(2000, 0);
  EXPECT_FALSE(l.feed(true, 1000));
  EXPECT_TRUE(l.pending());
  EXPECT_FALSE(l.tick(2999));
  EXPECT_FALSE(l.output());
  EXPECT_TRUE(l.tick(3000));
  EXPECT_TRUE(l.output());
  EXPECT_FALSE(l.pending());
  EXPECT_FALSE(l.tick(9000));
}

TEST(DelayLine, ASourceThatGoesOffBeforeTheOnDelayNeverTurnsItOn) {
  DelayLine l = line(2000, 0);
  l.feed(true, 1000);
  EXPECT_FALSE(l.feed(false, 2500));
  EXPECT_FALSE(l.pending());
  EXPECT_FALSE(l.tick(10000));
  EXPECT_FALSE(l.output());
  // And on again: the count starts from this edge, not the first one.
  l.feed(true, 4000);
  EXPECT_FALSE(l.tick(5999));
  EXPECT_TRUE(l.tick(6000));
}

TEST(DelayLine, TurnsOffOnceTheOffDelayHasPassedSinceTheSourceWentOff) {
  DelayLine l = line(0, 5000);
  EXPECT_TRUE(l.feed(true, 1000));  // no on delay: on at once
  EXPECT_TRUE(l.output());
  EXPECT_FALSE(l.feed(false, 2000));
  EXPECT_TRUE(l.output());
  EXPECT_FALSE(l.tick(6999));
  EXPECT_TRUE(l.output());
  EXPECT_TRUE(l.tick(7000));
  EXPECT_FALSE(l.output());
}

TEST(DelayLine, EveryNewPulseStartsTheOffDelayAgain) {
  DelayLine l = line(0, 5000);
  l.feed(true, 0);
  l.feed(false, 1000);
  // A motion sensor that pulses again 4 s later, and once more after that.
  EXPECT_FALSE(l.tick(5000));
  l.feed(true, 5000);
  EXPECT_FALSE(l.pending());  // on again: nothing counts while the source is on
  l.feed(false, 5500);
  EXPECT_FALSE(l.tick(10499));  // 5 s from the last off, not from the first
  EXPECT_TRUE(l.output());
  l.feed(true, 10000);
  l.feed(false, 10200);
  EXPECT_FALSE(l.tick(15199));
  EXPECT_TRUE(l.tick(15200));
  EXPECT_FALSE(l.output());
}

TEST(DelayLine, BothDelaysZeroFollowsTheSourceAtOnce) {
  DelayLine l = line(0, 0);
  EXPECT_TRUE(l.feed(true, 10));
  EXPECT_TRUE(l.output());
  EXPECT_TRUE(l.feed(false, 11));
  EXPECT_FALSE(l.output());
  EXPECT_TRUE(l.feed(true, 11));
  EXPECT_FALSE(l.pending());
}

TEST(DelayLine, TheSameLevelAgainChangesNothing) {
  DelayLine l = line(1000, 1000);
  l.feed(true, 0);
  // A repeat does not restart the count.
  EXPECT_FALSE(l.feed(true, 900));
  EXPECT_TRUE(l.tick(1000));
}

TEST(DelayLine, ARestartCountsTheWaitAgainFromNow) {
  DelayLine l = line(0, 5000);
  l.feed(true, 0);
  l.feed(false, 1000);
  // New delays while the off count runs: it counts again, under the new ones, from the save.
  l.set_delays(0, 2000);
  EXPECT_FALSE(l.restart(false, 4000));
  EXPECT_FALSE(l.tick(5999));
  EXPECT_TRUE(l.tick(6000));
  // A restart at the output's level leaves nothing to count.
  EXPECT_FALSE(l.restart(false, 7000));
  EXPECT_FALSE(l.pending());
}

TEST(DelayLine, ALevelForgottenHoldsTheOutputUntilTheNextOne) {
  DelayLine l = line(0, 1000);
  l.feed(true, 0);
  l.forget_level();
  EXPECT_TRUE(l.output());
  EXPECT_FALSE(l.has_level());
  EXPECT_FALSE(l.tick(100000));
  // The first level after it counts as a fresh one, whatever it was before.
  EXPECT_FALSE(l.feed(true, 200000));
  EXPECT_FALSE(l.feed(false, 200000));
  EXPECT_TRUE(l.tick(201000));
}

TEST(DelayLine, TheCountSurvivesTheClockWrapping) {
  DelayLine l = line(0, 5000);
  l.feed(true, 0xFFFFF000u);
  l.feed(false, 0xFFFFF000u);
  EXPECT_FALSE(l.tick(0xFFFFFFFFu));
  EXPECT_FALSE(l.tick(0x00000387u));  // 4999 ms after the off
  EXPECT_TRUE(l.tick(0x00000388u));
}

TEST(DelayLine, ADayLongDelayIsCountedInFull) {
  const uint32_t day = MAX_DELAY_S * 1000;
  DelayLine l = line(day, day);
  l.feed(true, 0);
  EXPECT_FALSE(l.tick(day - 1));
  EXPECT_TRUE(l.tick(day));
  l.feed(false, day);
  EXPECT_FALSE(l.tick(2 * day - 1));
  EXPECT_TRUE(l.tick(2 * day));
}

TEST(DelayLine, AResetIsOffWithNothingHeard) {
  DelayLine l = line(0, 1000);
  l.feed(true, 0);
  l.feed(false, 10);
  l.reset();
  EXPECT_FALSE(l.output());
  EXPECT_FALSE(l.pending());
  EXPECT_FALSE(l.has_level());
}

}  // namespace esphome::virtual_inputs::testing
