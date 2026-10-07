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

// The rhythm goes on through the point where a 32-bit millis() would wrap.
TEST(DutyCycler, KeepsItsRhythmPastTheMillisWrap) {
  DutyCycler duty;
  duty.set_period(1000);
  duty.set_duty(0.5f);
  const uint64_t before_wrap = MILLIS_WRAP - 1024;
  duty.start(before_wrap);

  EXPECT_TRUE(duty.update(before_wrap));
  EXPECT_TRUE(duty.update(before_wrap + 499));
  EXPECT_FALSE(duty.update(before_wrap + 500));
  EXPECT_TRUE(duty.update(before_wrap + 1000));
  EXPECT_FALSE(duty.update(before_wrap + 1500));
  EXPECT_TRUE(duty.update(before_wrap + 2000)) << "past the wrap";
  EXPECT_FALSE(duty.update(before_wrap + 2500));
}

// After a gap longer than the wrap the phase is still counted from the start: in 32 bits it
// would be off by the 296 ms that 2^32 ms leave over a whole number of periods.
TEST(DutyCycler, KeepsItsPhaseAcrossAGapLongerThanTheMillisWrap) {
  DutyCycler duty;
  duty.set_period(1000);
  duty.set_duty(0.5f);
  duty.start(0);
  ASSERT_TRUE(duty.update(0));

  EXPECT_FALSE(duty.update(MILLIS_WRAP + 250)) << "546 ms into a period";
  EXPECT_FALSE(duty.update(MILLIS_WRAP + 703));
  EXPECT_TRUE(duty.update(MILLIS_WRAP + 704)) << "a new period";
  EXPECT_FALSE(duty.update(MILLIS_WRAP + 1204));
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
  EXPECT_EQ(RelayWait::NONE, claim.wait());
  EXPECT_TRUE(claim.request(false, 4999)) << "still inside min_on";
  EXPECT_EQ(RelayWait::MIN_ON, claim.wait());
  EXPECT_FALSE(claim.request(false, 5000)) << "min_on elapsed";
  EXPECT_EQ(RelayWait::NONE, claim.wait());
  EXPECT_EQ(2, relay.writes);
}

TEST(RelayClaim, MinimumOffTimeHoldsTheRelayOpen) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(0, 5000);

  claim.request(true, 0);
  claim.request(false, 0);
  EXPECT_FALSE(claim.request(true, 4999));
  EXPECT_EQ(RelayWait::MIN_OFF, claim.wait());
  EXPECT_TRUE(claim.request(true, 5000));
  EXPECT_EQ(RelayWait::NONE, claim.wait());
}

TEST(RelayClaim, ForceOffIgnoresMinimumOnTime) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(60000, 0);

  claim.request(true, 0);
  claim.request(false, 5);
  ASSERT_EQ(RelayWait::MIN_ON, claim.wait());
  claim.force_off(10, false);
  EXPECT_FALSE(claim.state()) << "a safety cut-out cannot wait out a dwell floor";
  EXPECT_FALSE(relay.state);
  EXPECT_EQ(RelayWait::NONE, claim.wait());
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
// claim believes it open. It went where the demand goes, so it is no move against the claim.
TEST(RelayClaim, ARelayClosedElsewhereInsideMinOffIsOpenedEvenAsTheDemandTurnsOn) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(0, 60000);
  claim.request(true, 0);
  claim.request(false, 1000);
  relay.turn_on();  // someone else, 1 s into min_off

  EXPECT_FALSE(claim.request(true, 2000));
  EXPECT_FALSE(relay.state) << "the claim and the switch agree";
  EXPECT_EQ(0u, claim.moves());
  EXPECT_FALSE(claim.request(true, 61999)) << "opened again at 2 s, so min_off runs to 62 s";
  EXPECT_FALSE(relay.state);
  EXPECT_TRUE(claim.request(true, 62000));
  EXPECT_TRUE(relay.state);
}

// Uncounted, a close toward the demand inside min_off still makes the next one paced: the relay
// goes back once, then stays where the writer and the demand both want it, with no chatter.
TEST(RelayClaim, ASecondCloseTowardTheDemandInsideMinOffStays) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(0, 3600000);
  claim.request(true, 0);
  claim.request(false, 1000);
  relay.turn_on();
  ASSERT_FALSE(claim.request(true, 2000));
  const int writes = relay.writes;
  for (uint32_t t = 3000; t <= 10000; t += 1000) {
    if (!relay.state)
      relay.turn_on();
    EXPECT_TRUE(claim.request(true, t)) << "at " << t;
  }
  EXPECT_EQ(writes + 1, relay.writes) << "only the writer's own close";
  EXPECT_EQ(0u, claim.moves());
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
  claim.force_off(0, false);
  ASSERT_FALSE(relay.state);

  relay.turn_on();  // by hand, while the claim believes it open
  ASSERT_TRUE(relay.state);
  const int writes = relay.writes;

  claim.force_off(10, false);
  EXPECT_FALSE(relay.state) << "compared against the switch, not against the belief";
  EXPECT_EQ(writes + 1, relay.writes);

  claim.force_off(20, false);
  EXPECT_EQ(writes + 1, relay.writes) << "an open relay is left alone";
}

// Opening a relay that is open moves nothing, so it starts no dwell: the first request still
// lands at once, min_off or not.
TEST(RelayClaim, ForceOffOnAnOpenRelayLeavesTheFirstRequestFree) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(0, 60000);
  claim.force_off(1000, false);
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
  claim.force_off(2000, false);
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
    before.force_off(1000, false);
    ASSERT_TRUE(before.last_switching(&last));
  }
  EXPECT_FALSE(last.on);
  EXPECT_EQ(1000u, last.ms);

  RelayClaim after(&relay, "summer");
  after.set_dwell(0, 5000);
  after.resume(last);
  EXPECT_FALSE(after.request(true, 5999)) << "opened at 1 s, min_off 5 s";
  EXPECT_EQ(RelayWait::MIN_OFF, after.wait());
  after.resume(last);
  EXPECT_EQ(RelayWait::NONE, after.wait()) << "nothing asked of it since";
  EXPECT_TRUE(after.request(true, 6000));
}

TEST(RelayClaim, AResumedClaimStillCutsOutAtOnce) {
  FakeSwitch relay;
  relay.turn_on();
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(60000, 0);
  claim.resume({true, 0});
  claim.force_off(10, false);
  EXPECT_FALSE(relay.state) << "a safety cut-out cannot wait out a dwell floor";
}

TEST(RelayClaim, ChangesHandsWithoutMoving) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "winter");
  claim.set_dwell(60000, 0);
  claim.request(true, 0);
  claim.request(false, 5);
  ASSERT_EQ(RelayWait::MIN_ON, claim.wait());
  claim.set_owner("summer");
  EXPECT_EQ("summer", claim.owner());
  EXPECT_EQ(RelayWait::NONE, claim.wait()) << "what winter asked for is not summer's wait";
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

// The figures the docs give.
TEST(RelayClaim, ContestLimits) {
  EXPECT_EQ(5u, CONTEST_MOVES);
  EXPECT_EQ(600000u, CONTEST_QUIET_MS);
  EXPECT_EQ(10000u, PUT_BACK_FLOOR_MS);
}

// A writer that keeps at it: the first move goes back at once, the second only once the relay
// has stayed closed for min_on, since it did close.
TEST(RelayClaim, ASecondCloseIsPutBackOnceMinOnIsOver) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(30000, 45000);
  claim.request(false, 0);

  relay.turn_on();
  EXPECT_FALSE(claim.request(false, 1000)) << "the first move goes back at once";
  EXPECT_FALSE(relay.state);
  EXPECT_EQ(1u, claim.moves());

  relay.turn_on();
  EXPECT_TRUE(claim.request(false, 2000)) << "the second stays where it was moved";
  EXPECT_TRUE(relay.state);
  EXPECT_EQ(2u, claim.moves());
  EXPECT_TRUE(claim.request(false, 31999));
  EXPECT_EQ(RelayWait::NONE, claim.wait()) << "held for the put-back, not for its own min_on";
  EXPECT_FALSE(claim.request(false, 32000)) << "30 s after the close at 2 s";
  EXPECT_FALSE(relay.state);
  EXPECT_EQ(2u, claim.moves()) << "the put-back is no move";
}

// The same for a relay opened from elsewhere: it stays open for min_off.
TEST(RelayClaim, ASecondOpenIsPutBackOnceMinOffIsOver) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(30000, 45000);
  claim.request(true, 0);

  relay.turn_off();
  EXPECT_TRUE(claim.request(true, 1000));
  relay.turn_off();
  EXPECT_FALSE(claim.request(true, 2000));
  EXPECT_FALSE(claim.request(true, 46999));
  EXPECT_EQ(RelayWait::NONE, claim.wait()) << "held for the put-back, not for its own min_off";
  EXPECT_TRUE(claim.request(true, 47000)) << "45 s after the open at 2 s";
  EXPECT_TRUE(relay.state);
}

// With no dwell at all a writer would still get a switch per pass; the floor makes it one per
// 10 s. The first move goes back at once all the same.
TEST(RelayClaim, AZeroDwellStillWaitsTheFloor) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.request(true, 0);

  relay.turn_off();
  EXPECT_TRUE(claim.request(true, 1000));
  relay.turn_off();
  EXPECT_FALSE(claim.request(true, 2000));
  EXPECT_FALSE(claim.request(true, 2000 + PUT_BACK_FLOOR_MS - 1));
  EXPECT_TRUE(claim.request(true, 2000 + PUT_BACK_FLOOR_MS));

  claim.request(false, 20000);
  relay.turn_on();
  EXPECT_TRUE(claim.request(false, 21000)) << "a close waits the floor too";
  EXPECT_TRUE(claim.request(false, 21000 + PUT_BACK_FLOOR_MS - 1));
  EXPECT_FALSE(claim.request(false, 21000 + PUT_BACK_FLOOR_MS));
}

// Ten minutes of quiet after a put-back forget the moves before it, counted from the put-back
// rather than the move; the next move then goes back at once again.
TEST(RelayClaim, QuietAfterAPutBackForgetsTheMoves) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.request(true, 0);
  relay.turn_off();
  claim.request(true, 1000);
  ASSERT_EQ(1u, claim.moves());

  relay.turn_off();
  EXPECT_FALSE(claim.request(true, 1000 + CONTEST_QUIET_MS - 1)) << "a moment short of the quiet";
  EXPECT_EQ(2u, claim.moves());
  const uint32_t put_back = 1000 + CONTEST_QUIET_MS - 1 + PUT_BACK_FLOOR_MS;
  ASSERT_TRUE(claim.request(true, put_back));

  claim.request(true, put_back + CONTEST_QUIET_MS - 1);
  EXPECT_EQ(2u, claim.moves());
  claim.request(true, put_back + CONTEST_QUIET_MS);
  EXPECT_EQ(0u, claim.moves());

  relay.turn_off();
  EXPECT_TRUE(claim.request(true, put_back + CONTEST_QUIET_MS + 1000)) << "back at once";
  EXPECT_EQ(1u, claim.moves());
}

// Five moves without the quiet in between make the relay contested, and a pending put-back
// keeps it so however long it waits; it clears ten minutes after the last put-back.
TEST(RelayClaim, TheFifthMoveContestsTheRelay) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.request(true, 0);
  uint32_t t = 0;
  for (uint32_t n = 1; n <= CONTEST_MOVES; n++) {
    t += 1000;
    relay.turn_off();
    claim.request(true, t);
    EXPECT_EQ(n, claim.moves());
    EXPECT_EQ(n == CONTEST_MOVES, claim.contested(t)) << "move " << n;
    t += PUT_BACK_FLOOR_MS;
    claim.request(true, t);
    ASSERT_TRUE(relay.state) << "put back after move " << n;
  }
  EXPECT_TRUE(claim.contested(t + CONTEST_QUIET_MS - 1));
  EXPECT_FALSE(claim.contested(t + CONTEST_QUIET_MS)) << "clears by itself";
}

TEST(RelayClaim, APendingPutBackKeepsTheRelayContested) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(0, 3600000);
  claim.request(true, 0);
  uint32_t t = 0;
  for (uint32_t n = 1; n < CONTEST_MOVES; n++) {
    relay.turn_off();
    claim.request(true, t += 1000);
    relay.turn_on();
    claim.request(true, t += 1000);
  }
  relay.turn_off();
  claim.request(true, t += 1000);
  ASSERT_EQ(CONTEST_MOVES, claim.moves());
  EXPECT_FALSE(relay.state) << "held open for its hour of min_off";
  EXPECT_TRUE(claim.contested(t + CONTEST_QUIET_MS)) << "nothing was put back yet";
  claim.request(true, t + CONTEST_QUIET_MS);
  EXPECT_EQ(CONTEST_MOVES, claim.moves()) << "nor forgotten";
}

// Moved where the demand may go now, a relay stays and nothing is counted. From the second move
// on, a move where the demand goes stays even inside the dwell: nothing is undone for its own
// sake, so the writer and the thermostat agree and nothing chatters.
TEST(RelayClaim, AMoveWhereTheDemandGoesIsNotCounted) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(5000, 5000);
  claim.request(true, 0);
  claim.request(false, 5000);
  relay.turn_on();
  EXPECT_TRUE(claim.request(true, 10000));
  EXPECT_EQ(0u, claim.moves()) << "min_off was over";

  relay.turn_off();
  ASSERT_TRUE(claim.request(true, 11000));
  ASSERT_EQ(1u, claim.moves());
  EXPECT_TRUE(claim.request(false, 12000)) << "min_on holds it closed until 16 s";
  relay.turn_off();
  const int writes = relay.writes;
  EXPECT_FALSE(claim.request(false, 13000));
  EXPECT_FALSE(relay.state);
  EXPECT_EQ(writes, relay.writes) << "nothing written";
  EXPECT_EQ(1u, claim.moves());
}

// While a move waits for its put-back, the writer putting it back itself saves the thermostat
// a switch and is no move against it; the quiet starts there.
TEST(RelayClaim, AMoveUndoneElsewhereBeforeItsPutBackIsNotCounted) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.request(true, 0);
  relay.turn_off();
  claim.request(true, 1000);
  relay.turn_off();
  ASSERT_FALSE(claim.request(true, 2000));

  relay.turn_on();
  const int writes = relay.writes;
  EXPECT_TRUE(claim.request(true, 3000));
  EXPECT_EQ(writes, relay.writes) << "nothing written";
  EXPECT_EQ(2u, claim.moves());
  claim.request(true, 3000 + CONTEST_QUIET_MS);
  EXPECT_EQ(0u, claim.moves());
}

// The demand coming round to where the relay was moved leaves nothing to put back, and the
// thermostat's own next switching waits for its own dwell alone.
TEST(RelayClaim, APutBackTheDemandNoLongerWantsIsDropped) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.request(true, 0);
  relay.turn_off();
  claim.request(true, 1000);
  relay.turn_off();
  ASSERT_FALSE(claim.request(true, 2000));

  EXPECT_FALSE(claim.request(false, 3000));
  EXPECT_TRUE(claim.request(true, 4000)) << "min_off is 0, and no floor is pending";
  claim.request(true, 3000 + CONTEST_QUIET_MS);
  EXPECT_EQ(0u, claim.moves()) << "the quiet ran from 3 s";
}

// A boot that restored the relay closed is no move from elsewhere: it is opened, and counted
// nothing, whether the first look is a request or a cut-out.
TEST(RelayClaim, TheFirstLookAfterAResumeCountsNothing) {
  for (bool cut_out : {false, true}) {
    SCOPED_TRACE(cut_out ? "force_off" : "request");
    FakeSwitch relay;
    relay.turn_on();
    RelayClaim claim(&relay, "boiler");
    claim.resume({false, 0});
    if (cut_out) {
      claim.force_off(1000, false);
    } else {
      claim.request(false, 1000);
    }
    EXPECT_FALSE(relay.state);
    EXPECT_EQ(0u, claim.moves());

    relay.turn_on();
    EXPECT_FALSE(claim.request(false, 2000)) << "the next move is the first, back at once";
    EXPECT_EQ(1u, claim.moves());
  }
}

// A new holder, or a claim carried on from an earlier one, has no moves held against it, and a
// put-back that was pending waits for the dwell alone.
TEST(RelayClaim, AHandOverOrAResumeForgetsTheMoves) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "winter");
  claim.request(true, 0);
  relay.turn_off();
  claim.request(true, 1000);
  relay.turn_off();
  claim.request(true, 2000);
  ASSERT_EQ(2u, claim.moves());

  claim.set_owner("summer");
  EXPECT_EQ(0u, claim.moves());
  EXPECT_TRUE(claim.request(true, 3000)) << "no floor";
  relay.turn_off();
  EXPECT_TRUE(claim.request(true, 4000)) << "the next move is the first again";
  relay.turn_off();
  claim.request(true, 5000);
  ASSERT_EQ(2u, claim.moves());

  RelaySwitching last;
  ASSERT_TRUE(claim.last_switching(&last));
  claim.resume(last);
  EXPECT_EQ(0u, claim.moves());
  EXPECT_FALSE(claim.contested(5000));
}

// A relay opened from elsewhere while the claim holds it closed is where a cut-out wants it: it
// stays open and is no move against the thermostat.
TEST(RelayClaim, ACutOutKeepsARelayOpenedElsewhereOpen) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(60000, 0);
  claim.request(true, 0);
  relay.turn_off();
  claim.force_off(1000, false);
  EXPECT_FALSE(relay.state);
  EXPECT_FALSE(claim.state());
  EXPECT_EQ(0u, claim.moves());
}

// Without a switch to read, the claim's own belief is all there is: nothing ever moves it.
TEST(RelayClaim, AClaimWithoutASwitchSeesNoMoves) {
  RelayClaim claim(nullptr, "boiler");
  claim.set_dwell(5000, 0);
  EXPECT_TRUE(claim.request(true, 0));
  EXPECT_TRUE(claim.request(false, 4999));
  claim.force_off(5000, false);
  EXPECT_FALSE(claim.state());
  EXPECT_EQ(0u, claim.moves());
}

// Over-temperature or no reading: a relay closed from elsewhere is opened on every pass, with no
// pacing, and each close still counts.
TEST(RelayClaim, ACutOutOpensEveryCloseAtOnce) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(60000, 60000);
  claim.resume({false, 0});
  claim.force_off(1000, false);
  for (uint32_t t = 2000; t <= 7000; t += 1000) {
    relay.turn_on();
    claim.force_off(t, false);
    EXPECT_FALSE(relay.state) << "at " << t;
  }
  EXPECT_EQ(6u, claim.moves());
  EXPECT_TRUE(claim.contested(7000));
}

// A cut-out does not wait for a put-back that is due later either.
TEST(RelayClaim, ACutOutOpensAPacedCloseAtOnce) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(30000, 0);
  claim.request(false, 0);
  relay.turn_on();
  claim.request(false, 1000);
  relay.turn_on();
  claim.request(false, 2000);
  ASSERT_TRUE(relay.state) << "held closed for min_on";

  claim.force_off(3000, true);
  EXPECT_TRUE(relay.state) << "a paced off, as mode off asks, leaves it closed until min_on is over";
  claim.force_off(3000, false);
  EXPECT_FALSE(relay.state);
  EXPECT_EQ(2u, claim.moves());
}

// Held open in mode off with a reading, the relay is put back as a request puts it back: the
// second close waits out min_on.
TEST(RelayClaim, HeldOpenTheSecondCloseWaitsOutMinOn) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(30000, 0);
  claim.resume({false, 0});
  claim.force_off(1000, true);
  relay.turn_on();
  claim.force_off(2000, true);
  EXPECT_FALSE(relay.state) << "the first goes back at once";
  relay.turn_on();
  claim.force_off(3000, true);
  EXPECT_TRUE(relay.state) << "the second stays for min_on";
  claim.force_off(32999, true);
  EXPECT_TRUE(relay.state);
  claim.force_off(33000, true);
  EXPECT_FALSE(relay.state);
  EXPECT_EQ(2u, claim.moves());
}

// An open waiting to be put back that a cut-out finds open has nothing left to put back.
TEST(RelayClaim, HeldOpenAPendingOpenIsSettled) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.request(true, 0);
  relay.turn_off();
  claim.request(true, 1000);
  relay.turn_off();
  ASSERT_FALSE(claim.request(true, 2000));

  claim.force_off(3000, true);
  EXPECT_FALSE(relay.state);
  claim.force_off(3000 + CONTEST_QUIET_MS, true);
  EXPECT_EQ(0u, claim.moves()) << "the quiet ran from 3 s";
}

// A cut-out puts a close back on every pass: past CONTEST_MOVES only every doubling of the count
// is logged, 1 to 5, 8, 16, 32 and 64 of 75.
TEST(RelayClaim, AMoveOnEveryPassIsLoggedAtEachDoubling) {
  FakeSwitch &relay = entities().relay3;
  relay.publish_state(false);
  RelayClaim claim(&relay, "boiler");
  claim.request(false, 0);
  LogCapture::instance().clear();
  for (uint32_t t = 1000; t <= 75000; t += 1000) {
    relay.turn_on();
    claim.force_off(t, false);
  }
  ASSERT_EQ(75u, claim.moves());
  std::vector<unsigned long> logged;
  for (const auto &line : LogCapture::instance().lines) {
    const auto at = line.find("moved from elsewhere (");
    if (at != std::string::npos)
      logged.push_back(std::stoul(line.substr(at + 22)));
  }
  EXPECT_EQ((std::vector<unsigned long>{1, 2, 3, 4, 5, 8, 16, 32, 64}), logged);
}

// Each move against the thermostat is logged with the relay, the count, and when it goes back.
TEST(RelayClaim, EachMoveAgainstItIsLogged) {
  FakeSwitch &relay = entities().relay3;
  relay.publish_state(false);
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(45000, 0);
  claim.request(false, 0);
  LogCapture::instance().clear();

  relay.turn_on();
  claim.request(false, 1000);
  EXPECT_TRUE(LogCapture::instance().has("'boiler': relay 'Relay 3' moved from elsewhere (1), put back"));
  relay.turn_on();
  claim.request(false, 2000);
  EXPECT_TRUE(LogCapture::instance().has("'boiler': relay 'Relay 3' moved from elsewhere (2), put back in 45 s"));
  relay.publish_state(false);
}

// Neither the floor nor the quiet notices the point where a 32-bit millis() would wrap.
TEST(RelayClaim, PacingGoesOnPastTheMillisWrap) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  const uint64_t t0 = MILLIS_WRAP - 5000;
  claim.request(true, t0);
  relay.turn_off();
  claim.request(true, t0 + 1000);
  relay.turn_off();
  ASSERT_FALSE(claim.request(true, t0 + 2000));
  EXPECT_FALSE(claim.request(true, t0 + 2000 + PUT_BACK_FLOOR_MS - 1)) << "across the wrap";
  const uint64_t put_back = t0 + 2000 + PUT_BACK_FLOOR_MS;
  ASSERT_GT(put_back, MILLIS_WRAP);
  EXPECT_TRUE(claim.request(true, put_back));

  claim.request(true, put_back + CONTEST_QUIET_MS - 1);
  EXPECT_EQ(2u, claim.moves());
  claim.request(true, put_back + CONTEST_QUIET_MS);
  EXPECT_EQ(0u, claim.moves());
}

// A relay left open for 49.7 days and a second has had its ten minutes of min_off: in 32 bits
// the second would read as the time since it opened, and it would wait out the dwell again.
TEST(RelayClaim, ARelayIdlePastTheMillisWrapSwitchesAtOnce) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.set_dwell(600000, 600000);
  claim.request(true, 0);
  ASSERT_FALSE(claim.request(false, 600000));
  EXPECT_TRUE(claim.request(true, 600000 + MILLIS_WRAP + 1000));

  // The same from a switching an earlier claim made, as a boot that opened the relay leaves it.
  relay.publish_state(false);
  RelayClaim next(&relay, "boiler");
  next.set_dwell(600000, 600000);
  next.resume(RelaySwitching{false, 0});
  EXPECT_TRUE(next.request(true, MILLIS_WRAP + 1000));
}

// Forgotten, not just outgrown: once the quiet ran out on a pass, the next move starts a new
// count instead of contesting the relay again.
TEST(RelayClaim, AContestIsForgottenForGood) {
  FakeSwitch relay;
  RelayClaim claim(&relay, "boiler");
  claim.request(true, 0);
  uint64_t t = 0;
  for (uint32_t n = 1; n <= CONTEST_MOVES; n++) {
    relay.turn_off();
    claim.request(true, t += 1000);
    claim.request(true, t += PUT_BACK_FLOOR_MS);
  }
  ASSERT_TRUE(claim.contested(t));
  claim.request(true, t += CONTEST_QUIET_MS);
  relay.turn_off();
  EXPECT_TRUE(claim.request(true, t += 1000)) << "back at once";
  EXPECT_EQ(1u, claim.moves());
  EXPECT_FALSE(claim.contested(t));
}

}  // namespace esphome::climate_hub::testing
