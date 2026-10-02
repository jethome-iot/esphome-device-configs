// Slow PWM and relay dwell, driven by an injected clock: both are pure timing, and neither is
// observable on a device without waiting minutes for it.
#include "common.h"
#include "esphome/components/climate_hub/duty_cycler.h"
#include "esphome/components/climate_hub/relay_claim.h"

namespace esphome::climate_hub::testing {

TEST(DutyCycler, RailsAreExact) {
  DutyCycler duty;
  duty.set_period(1000);
  duty.start(0);

  duty.set_duty(0.f);
  for (uint32_t t = 0; t < 3000; t += 100)
    EXPECT_FALSE(duty.update(t)) << "at " << t;

  duty.set_duty(1.f);
  for (uint32_t t = 0; t < 3000; t += 100)
    EXPECT_TRUE(duty.update(t)) << "at " << t;
}

TEST(DutyCycler, OutOfRangeDutyIsClamped) {
  DutyCycler duty;
  duty.set_duty(5.f);
  EXPECT_FLOAT_EQ(1.f, duty.duty());
  duty.set_duty(-5.f);
  EXPECT_FLOAT_EQ(0.f, duty.duty());
  duty.set_duty(NAN);
  EXPECT_FLOAT_EQ(0.f, duty.duty());
}

TEST(DutyCycler, HalfDutyIsOnForHalfThePeriod) {
  DutyCycler duty;
  duty.set_period(1000);
  duty.set_duty(0.5f);
  duty.start(0);

  EXPECT_TRUE(duty.update(0));
  EXPECT_TRUE(duty.update(499));
  EXPECT_FALSE(duty.update(500));
  EXPECT_FALSE(duty.update(999));
  EXPECT_TRUE(duty.update(1000));
}

// millis() wraps every 49.7 days and the duty cycle must not notice.
TEST(DutyCycler, SurvivesMillisRollover) {
  DutyCycler duty;
  duty.set_period(1000);
  duty.set_duty(0.5f);
  const uint32_t before_wrap = 0xFFFFFC00u;
  duty.start(before_wrap);

  EXPECT_TRUE(duty.update(before_wrap));
  EXPECT_TRUE(duty.update(before_wrap + 499));
  EXPECT_FALSE(duty.update(before_wrap + 500));
  EXPECT_TRUE(duty.update(before_wrap + 1000));
  EXPECT_FALSE(duty.update(before_wrap + 1500));
}

// A loop that stalled for minutes resyncs in one call, not one period per iteration.
TEST(DutyCycler, CatchesUpAcrossManyMissedPeriods) {
  DutyCycler duty;
  duty.set_period(1000);
  duty.set_duty(0.5f);
  duty.start(0);
  EXPECT_TRUE(duty.update(0));

  EXPECT_TRUE(duty.update(100000));
  EXPECT_FALSE(duty.update(100600));
}

TEST(DutyCycler, ResyncsOnPhaseAfterAVeryLongGap) {
  DutyCycler duty;
  duty.set_period(1000);
  duty.set_duty(0.5f);
  duty.start(0);

  // 0xF0000000 is 840 ms into a period, past the on-fraction.
  EXPECT_FALSE(duty.update(0xF0000000u));
  // 200 ms later the period has rolled over, so it is on again.
  EXPECT_TRUE(duty.update(0xF0000000u + 200));
  EXPECT_FALSE(duty.update(0xF0000000u + 700));
}

// A Save that keeps the period keeps the rhythm; a new period starts afresh.
TEST(DutyCycler, OnlyANewPeriodRestartsThePhase) {
  DutyCycler duty;
  duty.set_period(1000);
  duty.set_duty(0.5f);
  EXPECT_TRUE(duty.update(0));
  duty.set_period(1000);
  EXPECT_FALSE(duty.update(600)) << "still the period that began at 0";
  duty.set_period(2000);
  EXPECT_TRUE(duty.update(700)) << "a new period begins here";
  EXPECT_FALSE(duty.update(1700));
  duty.set_period(0);
  EXPECT_TRUE(duty.update(1800)) << "a zero period is one millisecond";
}

TEST(RelayClaim, FirstRequestAppliesImmediately) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(10000, 10000);

  EXPECT_TRUE(claim.request(true, 0));
  EXPECT_TRUE(claim.state());
  EXPECT_EQ(1, relay.writes);
}

TEST(RelayClaim, MinimumOnTimeHoldsTheRelayClosed) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(5000, 0);

  claim.request(true, 0);
  EXPECT_TRUE(claim.request(false, 4999)) << "still inside min_on";
  EXPECT_FALSE(claim.request(false, 5000)) << "min_on elapsed";
  EXPECT_EQ(2, relay.writes);
}

TEST(RelayClaim, MinimumOffTimeHoldsTheRelayOpen) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(0, 5000);

  claim.request(true, 0);
  claim.request(false, 0);
  EXPECT_FALSE(claim.request(true, 4999));
  EXPECT_TRUE(claim.request(true, 5000));
}

TEST(RelayClaim, ForceOffIgnoresMinimumOnTime) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(60000, 0);

  claim.request(true, 0);
  claim.force_off(10);
  EXPECT_FALSE(claim.state()) << "a safety cut-out cannot wait out a dwell floor";
  EXPECT_FALSE(relay.state);
}

// A bang-bang output holds one demand for hours, so a relay switched by hand would otherwise
// stay wrong indefinitely rather than until the next tick.
TEST(RelayClaim, TakesTheRelayBackWhenSomethingElseMovedIt) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(60000, 60000);

  claim.request(true, 0);
  ASSERT_TRUE(relay.state);

  relay.turn_off();  // someone else, from another surface
  ASSERT_FALSE(relay.state);

  claim.request(true, 10);
  EXPECT_TRUE(relay.state) << "re-asserted even inside the dwell window, since the demand never changed";
  EXPECT_TRUE(claim.state());
}

// The put-back is a move like any other: the dwell runs from it, not from the claim's last one.
TEST(RelayClaim, TheDwellCountsFromThePutBack) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(60000, 60000);
  claim.request(true, 0);
  relay.turn_off();  // someone else

  EXPECT_TRUE(claim.request(true, 10000));
  EXPECT_TRUE(claim.request(false, 69999)) << "closed again at 10 s, so min_on runs to 70 s";
  EXPECT_FALSE(claim.request(false, 70000));
  EXPECT_FALSE(relay.state);
}

// Closed from elsewhere inside min_off, on the very tick the demand turns on: it is opened
// again and min_off starts over, rather than left closed for the rest of the hold while the
// claim believes it open.
TEST(RelayClaim, ARelayClosedElsewhereInsideMinOffIsOpenedEvenAsTheDemandTurnsOn) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(0, 60000);
  claim.request(true, 0);
  claim.request(false, 1000);
  relay.turn_on();  // someone else, 1 s into min_off

  EXPECT_FALSE(claim.request(true, 2000));
  EXPECT_FALSE(relay.state) << "the claim and the switch agree";
  EXPECT_FALSE(claim.request(true, 61999)) << "opened again at 2 s, so min_off runs to 62 s";
  EXPECT_FALSE(relay.state);
  EXPECT_TRUE(claim.request(true, 62000));
  EXPECT_TRUE(relay.state);
}

// Closed from elsewhere once min_off is over, as the demand turns on: already where the demand
// goes, it stays, with no open and close in between.
TEST(RelayClaim, ARelayMovedWhereTheDemandMayGoStays) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(5000, 5000);
  claim.request(true, 0);
  claim.request(false, 5000);
  relay.turn_on();  // someone else, 5 s after it opened
  const int writes = relay.writes;

  EXPECT_TRUE(claim.request(true, 10000));
  EXPECT_TRUE(relay.state);
  EXPECT_EQ(writes + 1, relay.writes) << "one write that keeps it closed";
  EXPECT_TRUE(claim.request(false, 14999)) << "min_on runs from 10 s";
  EXPECT_FALSE(claim.request(false, 15000));
}

// During a fault the claim already believes the relay open; one closed by hand in the meantime
// has to be opened again, which only the switch's own state can tell.
TEST(RelayClaim, ForceOffReopensARelayClosedByHand) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.force_off(0);
  ASSERT_FALSE(relay.state);

  relay.turn_on();  // by hand, while the claim believes it open
  ASSERT_TRUE(relay.state);
  const int writes = relay.writes;

  claim.force_off(10);
  EXPECT_FALSE(relay.state) << "compared against the switch, not against the belief";
  EXPECT_EQ(writes + 1, relay.writes);

  claim.force_off(20);
  EXPECT_EQ(writes + 1, relay.writes) << "an open relay is left alone";
}

// Opening a relay that is open moves nothing, so it starts no dwell: the first request still
// lands at once, min_off or not.
TEST(RelayClaim, ForceOffOnAnOpenRelayLeavesTheFirstRequestFree) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(0, 60000);
  claim.force_off(1000);
  EXPECT_EQ(0, relay.writes) << "nothing to open";
  RelaySwitching last;
  EXPECT_FALSE(claim.last_switching(&last)) << "nothing switched yet";

  EXPECT_TRUE(claim.request(true, 2000));
  EXPECT_TRUE(relay.state);
}

// What the hub hands out for a relay no claim has moved since boot: open since 0 ms. Keeping it
// open writes nothing and restarts nothing, and min_off counts from the boot.
TEST(RelayClaim, AClaimOpenSinceBootCountsMinOffFromTheBoot) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(0, 60000);
  claim.resume({false, 0});
  EXPECT_FALSE(claim.request(false, 1000));
  claim.force_off(2000);
  EXPECT_EQ(0, relay.writes) << "keeping an open relay open is no move";
  EXPECT_FALSE(claim.request(true, 59999));
  EXPECT_TRUE(claim.request(true, 60000));
  EXPECT_EQ(1, relay.writes);
}

// A claim made after another let the relay go carries on from where that one left it.
TEST(RelayClaim, ResumesTheDwellOfAnEarlierClaim) {
  FakeSwitch relay;
  RelaySwitching last;
  {
    RelayClaim before(&relay, "winter");
    EXPECT_FALSE(before.last_switching(&last)) << "nothing switched yet";
    before.request(true, 0);
    before.force_off(1000);
    ASSERT_TRUE(before.last_switching(&last));
  }
  EXPECT_FALSE(last.on);
  EXPECT_EQ(1000u, last.ms);

  RelayClaim after(&relay, "summer");
  after.set_dwell(0, 5000);
  after.resume(last);
  EXPECT_FALSE(after.request(true, 5999)) << "opened at 1 s, min_off 5 s";
  EXPECT_TRUE(after.request(true, 6000));
}

TEST(RelayClaim, AResumedClaimStillCutsOutAtOnce) {
  FakeSwitch relay;
  relay.turn_on();
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(60000, 0);
  claim.resume({true, 0});
  claim.force_off(10);
  EXPECT_FALSE(relay.state) << "a safety cut-out cannot wait out a dwell floor";
}

TEST(RelayClaim, ChangesHandsWithoutMoving) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "winter");
  claim.request(true, 0);
  claim.set_owner("summer");
  EXPECT_EQ("summer", claim.owner());
  EXPECT_TRUE(claim.request(true, 10));
  EXPECT_EQ(1, relay.writes);
}

TEST(RelayClaim, RepeatedIdenticalRequestsDoNotToggleTheRelay) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.request(true, 0);
  for (uint32_t t = 1; t < 100; t++)
    claim.request(true, t);
  EXPECT_EQ(1, relay.writes);
}

}  // namespace esphome::climate_hub::testing
