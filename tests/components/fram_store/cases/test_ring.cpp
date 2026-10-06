// Rings: appending, wrapping, finding the head after a reboot, and surviving damaged records.

#include "common.h"

namespace esphome::fram_store::testing {

TEST_F(FramStoreTest, RingAppendsAndReadsNewestFirst) {
  auto mounted = this->mount();

  for (uint64_t value = 1; value <= 3; value++) {
    ASSERT_TRUE(mounted->ring.append(value));
  }

  EXPECT_EQ(mounted->ring.count(), 3u);
  EXPECT_EQ(mounted->ring.total_appended(), 3u);

  for (uint16_t index = 0; index < 3; index++) {
    uint64_t read = 0;
    ASSERT_TRUE(mounted->ring.peek(index, read));
    EXPECT_EQ(read, 3u - index);
  }
}

TEST_F(FramStoreTest, RingWrapsAndDropsTheOldest) {
  auto mounted = this->mount();

  for (uint64_t value = 1; value <= RING_CAPACITY + 2; value++) {
    ASSERT_TRUE(mounted->ring.append(value));
  }

  EXPECT_EQ(mounted->ring.count(), RING_CAPACITY);
  EXPECT_EQ(mounted->ring.total_appended(), RING_CAPACITY + 2u);

  uint64_t newest = 0;
  ASSERT_TRUE(mounted->ring.peek(0, newest));
  EXPECT_EQ(newest, RING_CAPACITY + 2u);

  uint64_t oldest = 0;
  ASSERT_TRUE(mounted->ring.peek(RING_CAPACITY - 1, oldest));
  EXPECT_EQ(oldest, 3u);

  uint64_t past_the_end = 0;
  EXPECT_FALSE(mounted->ring.peek(RING_CAPACITY, past_the_end));
}

TEST_F(FramStoreTest, RingFindsItsHeadAfterARestart) {
  {
    auto mounted = this->mount();
    for (uint64_t value = 1; value <= RING_CAPACITY + 1; value++) {
      ASSERT_TRUE(mounted->ring.append(value));
    }
  }

  auto mounted = this->mount();
  EXPECT_FALSE(mounted->ring.used_sweep());
  EXPECT_EQ(mounted->ring.count(), RING_CAPACITY);
  EXPECT_EQ(mounted->ring.total_appended(), RING_CAPACITY + 1u);

  uint64_t newest = 0;
  ASSERT_TRUE(mounted->ring.peek(0, newest));
  EXPECT_EQ(newest, RING_CAPACITY + 1u);

  uint64_t next = 42;
  ASSERT_TRUE(mounted->ring.append(next));
  ASSERT_TRUE(mounted->ring.peek(0, next));
  EXPECT_EQ(next, 42u);
}

TEST_F(FramStoreTest, RingIgnoresATornRecord) {
  {
    auto mounted = this->mount();
    for (uint64_t value = 1; value <= 3; value++) {
      ASSERT_TRUE(mounted->ring.append(value));
    }
  }

  // Record 2 is the head; damaging its payload is what an interrupted append leaves.
  this->corrupt(ring_payload(2));

  auto mounted = this->mount();
  EXPECT_TRUE(mounted->ring.used_sweep());
  EXPECT_EQ(mounted->ring.count(), 2u);
  EXPECT_EQ(mounted->ring.total_appended(), 2u);

  uint64_t newest = 0;
  ASSERT_TRUE(mounted->ring.peek(0, newest));
  EXPECT_EQ(newest, 2u);

  // The next append lands on the torn record and heals the ring.
  uint64_t value = 9;
  ASSERT_TRUE(mounted->ring.append(value));
  ASSERT_TRUE(mounted->ring.peek(0, value));
  EXPECT_EQ(value, 9u);
}

// A ring filled exactly to capacity has no blank tail and has not wrapped -- the binary search has to
// recognise that shape rather than assume one of the other two.
TEST_F(FramStoreTest, RingExactlyFullFindsItsHead) {
  {
    auto mounted = this->mount();
    for (uint64_t value = 1; value <= RING_CAPACITY; value++) {
      ASSERT_TRUE(mounted->ring.append(value));
    }
  }

  auto mounted = this->mount();
  EXPECT_FALSE(mounted->ring.used_sweep());
  EXPECT_EQ(mounted->ring.count(), RING_CAPACITY);
  EXPECT_EQ(mounted->ring.total_appended(), RING_CAPACITY);

  uint64_t newest = 0;
  ASSERT_TRUE(mounted->ring.peek(0, newest));
  EXPECT_EQ(newest, RING_CAPACITY);
  uint64_t oldest = 0;
  ASSERT_TRUE(mounted->ring.peek(RING_CAPACITY - 1, oldest));
  EXPECT_EQ(oldest, 1u);
}

// A damaged sequence number breaks the monotonicity the binary search relies on; the head it lands on
// still has to be the real one, and the undamaged records around it still readable.
TEST_F(FramStoreTest, RingSurvivesADamagedSequenceNumber) {
  {
    auto mounted = this->mount();
    for (uint64_t value = 1; value <= RING_CAPACITY + 2; value++) {
      ASSERT_TRUE(mounted->ring.append(value));
    }
  }

  uint32_t garbage = 0xFFFFFFFF;
  ASSERT_TRUE(this->backend_.write(ring_entry(2), reinterpret_cast<const uint8_t *>(&garbage), sizeof(garbage)));

  auto mounted = this->mount();
  EXPECT_EQ(mounted->ring.total_appended(), RING_CAPACITY + 2u);

  uint64_t newest = 0;
  ASSERT_TRUE(mounted->ring.peek(0, newest));
  EXPECT_EQ(newest, RING_CAPACITY + 2u);
  uint64_t older = 0;
  ASSERT_TRUE(mounted->ring.peek(1, older));
  EXPECT_EQ(older, RING_CAPACITY + 1u);
}

TEST_F(FramStoreTest, RingPeekRejectsADamagedRecord) {
  auto mounted = this->mount();
  for (uint64_t value = 1; value <= 3; value++) {
    ASSERT_TRUE(mounted->ring.append(value));
  }

  this->corrupt(ring_payload(1));

  uint64_t newest = 0;
  EXPECT_TRUE(mounted->ring.peek(0, newest));
  uint64_t damaged = 0;
  EXPECT_FALSE(mounted->ring.peek(1, damaged));
}

// Damaged records are stepped over one pass at a time until a usable head turns up.
TEST_F(FramStoreTest, RingRecoversWhenEveryRecordIsDamaged) {
  {
    auto mounted = this->mount();
    for (uint64_t value = 1; value <= RING_CAPACITY; value++) {
      ASSERT_TRUE(mounted->ring.append(value));
    }
  }
  for (uint16_t index = 0; index < RING_CAPACITY; index++) {
    this->corrupt(ring_payload(index));
  }

  auto mounted = this->mount();
  EXPECT_TRUE(mounted->ring.is_initialised());
  EXPECT_EQ(mounted->ring.count(), 0u);

  uint64_t value = 77;
  ASSERT_TRUE(mounted->ring.append(value));
  mounted.reset();

  // The remount is the point: leaving the damaged records in place would let their higher sequence
  // numbers win the next scan and swallow this append.
  auto restarted = this->mount();
  EXPECT_EQ(restarted->ring.count(), 1u);
  ASSERT_TRUE(restarted->ring.peek(0, value));
  EXPECT_EQ(value, 77u);
}

TEST_F(FramStoreTest, RingRefusesToAppendWhenItCannotBeRead) {
  this->backend_.fail_reads_covering(RING_OFFSET);
  auto mounted = this->mount();

  EXPECT_FALSE(mounted->ring.is_initialised());
  uint64_t value = 1;
  EXPECT_FALSE(mounted->ring.append(value));
  // Erasing what it could not identify is as wrong as writing over it.
  EXPECT_FALSE(mounted->ring.clear());
}

TEST_F(FramStoreTest, RingRejectsTheWrongRecordSize) {
  auto mounted = this->mount();
  uint64_t stored = 3;
  ASSERT_TRUE(mounted->ring.append(stored));

  uint32_t too_small = 7;
  EXPECT_FALSE(mounted->ring.append(too_small));
  EXPECT_FALSE(mounted->ring.peek(0, too_small));
  EXPECT_EQ(mounted->ring.count(), 1u);
}

TEST_F(FramRingGeometryTest, ASingleRecordRingKeepsOnlyTheNewest) {
  Rig rig{128, RING_RECORD, 1};

  uint64_t value = 1;
  ASSERT_TRUE(rig.ring.append(value));
  value = 2;
  ASSERT_TRUE(rig.ring.append(value));

  EXPECT_EQ(rig.ring.count(), 1u);
  EXPECT_EQ(rig.ring.total_appended(), 2u);
  uint64_t read = 0;
  ASSERT_TRUE(rig.ring.peek(0, read));
  EXPECT_EQ(read, 2u);
  EXPECT_FALSE(rig.ring.peek(1, read));
}

// The whole point of the binary search: a long ring must cost a handful of reads at boot, not one per
// record. Without the bound this test passes on the sweep too.
TEST_F(FramRingGeometryTest, FindsTheHeadInAHandfulOfReads) {
  static const uint16_t CAPACITY = 64;
  FaultyBackend device{1408};
  {
    Rig rig{1408, RING_RECORD, CAPACITY, &device};
    for (uint64_t value = 1; value <= CAPACITY + 5; value++) {
      ASSERT_TRUE(rig.ring.append(value));
    }
  }

  device.reset_reads();
  Rig restarted{1408, RING_RECORD, CAPACITY, &device};

  EXPECT_FALSE(restarted.ring.used_sweep());
  EXPECT_EQ(restarted.ring.count(), CAPACITY);
  EXPECT_EQ(restarted.ring.total_appended(), CAPACITY + 5u);
  EXPECT_LT(device.reads(), 20u) << "binary search should not read anywhere near " << CAPACITY << " records";

  uint64_t newest = 0;
  ASSERT_TRUE(restarted.ring.peek(0, newest));
  EXPECT_EQ(newest, CAPACITY + 5u);
}

// A zeroed sequence word at index 0 is indistinguishable from an empty ring by that word alone. Treating
// it as empty would hide every surviving record and restart numbering from 1, orphaning them for good.
TEST_F(FramRingGeometryTest, AZeroedFirstRecordDoesNotHideTheRest) {
  static const uint16_t CAPACITY = 8;
  FaultyBackend device{512};
  {
    Rig rig{512, RING_RECORD, CAPACITY, &device};
    for (uint64_t value = 1; value <= CAPACITY + 4; value++) {
      ASSERT_TRUE(rig.ring.append(value));
    }
  }

  uint32_t zero = 0;
  ASSERT_TRUE(device.write(SLOT_OFFSET, reinterpret_cast<const uint8_t *>(&zero), sizeof(zero)));

  Rig restarted{512, RING_RECORD, CAPACITY, &device};
  EXPECT_TRUE(restarted.ring.used_sweep());
  EXPECT_EQ(restarted.ring.total_appended(), CAPACITY + 4u);
  EXPECT_GT(restarted.ring.count(), 1u);

  uint64_t newest = 0;
  ASSERT_TRUE(restarted.ring.peek(0, newest));
  EXPECT_EQ(newest, CAPACITY + 4u);
}

// A corrupt sequence number in the middle steers the binary search onto an older record whose own CRC is
// intact. Accepting it would move the head backwards and silently drop everything newer.
TEST_F(FramRingGeometryTest, ADamagedSequenceDoesNotMoveTheHeadBackwards) {
  static const uint16_t CAPACITY = 8;
  FaultyBackend device{512};
  {
    Rig rig{512, RING_RECORD, CAPACITY, &device};
    for (uint64_t value = 1; value <= CAPACITY + 6; value++) {
      ASSERT_TRUE(rig.ring.append(value));
    }
  }

  uint32_t stale = 1;
  uint16_t entry = FramRing::HEADER_SIZE + RING_RECORD;
  ASSERT_TRUE(device.write(static_cast<uint16_t>(SLOT_OFFSET + 3 * entry), reinterpret_cast<const uint8_t *>(&stale),
                           sizeof(stale)));

  Rig restarted{512, RING_RECORD, CAPACITY, &device};
  EXPECT_TRUE(restarted.ring.used_sweep());
  EXPECT_EQ(restarted.ring.total_appended(), CAPACITY + 6u);

  uint64_t newest = 0;
  ASSERT_TRUE(restarted.ring.peek(0, newest));
  EXPECT_EQ(newest, CAPACITY + 6u);
}

// A zeroed sequence in the middle of a filling ring looks exactly like the start of its blank tail.
// Accepting it would hide every record after the hole.
TEST_F(FramRingGeometryTest, AHoleInAFillingRingDoesNotHideTheTail) {
  static const uint16_t CAPACITY = 8;
  FaultyBackend device{512};
  {
    Rig rig{512, RING_RECORD, CAPACITY, &device};
    for (uint64_t value = 1; value <= 6; value++) {
      ASSERT_TRUE(rig.ring.append(value));
    }
  }

  uint32_t zero = 0;
  uint16_t entry = FramRing::HEADER_SIZE + RING_RECORD;
  ASSERT_TRUE(device.write(static_cast<uint16_t>(SLOT_OFFSET + 3 * entry), reinterpret_cast<const uint8_t *>(&zero),
                           sizeof(zero)));

  Rig restarted{512, RING_RECORD, CAPACITY, &device};
  EXPECT_EQ(restarted.ring.total_appended(), 6u);

  uint64_t newest = 0;
  ASSERT_TRUE(restarted.ring.peek(0, newest));
  EXPECT_EQ(newest, 6u);
}

// Four damaged records is the documented tolerance, so the fifth-newest record must still be reachable
// as a head -- with only four candidates kept, the ring would be wiped instead.
TEST_F(FramRingGeometryTest, SurvivesTheDocumentedNumberOfDamagedRecords) {
  static const uint16_t CAPACITY = 8;
  FaultyBackend device{512};
  {
    Rig rig{512, RING_RECORD, CAPACITY, &device};
    for (uint64_t value = 1; value <= CAPACITY; value++) {
      ASSERT_TRUE(rig.ring.append(value));
    }
  }

  // Records 8, 7, 6 and 5 are the four newest; damaging them leaves 4 as the newest intact one.
  uint8_t garbage[RING_RECORD];
  std::memset(garbage, 0x5A, sizeof(garbage));
  for (uint16_t index = CAPACITY - 4; index < CAPACITY; index++) {
    uint16_t payload = SLOT_OFFSET + index * (FramRing::HEADER_SIZE + RING_RECORD) + FramRing::HEADER_SIZE;
    ASSERT_TRUE(device.write(payload, garbage, sizeof(garbage)));
  }

  Rig restarted{512, RING_RECORD, CAPACITY, &device};
  EXPECT_TRUE(restarted.ring.used_sweep());
  EXPECT_EQ(restarted.ring.total_appended(), CAPACITY - 4u);

  uint64_t newest = 0;
  ASSERT_TRUE(restarted.ring.peek(0, newest));
  EXPECT_EQ(newest, CAPACITY - 4u);
}

// Wiping an unusable ring is what makes later appends visible again; if the wipe itself fails, the ring
// must not report itself usable -- appending into a half-erased ring loses the records silently.
TEST_F(FramRingGeometryTest, AFailedWipeLeavesTheRingUnusable) {
  static const uint16_t CAPACITY = 8;
  FaultyBackend device{512};
  {
    Rig rig{512, RING_RECORD, CAPACITY, &device};
    for (uint64_t value = 1; value <= CAPACITY; value++) {
      ASSERT_TRUE(rig.ring.append(value));
    }
  }
  for (uint16_t index = 0; index < CAPACITY; index++) {
    uint8_t garbage[RING_RECORD];
    std::memset(garbage, 0x5A, sizeof(garbage));
    uint16_t payload = SLOT_OFFSET + index * (FramRing::HEADER_SIZE + RING_RECORD) + FramRing::HEADER_SIZE;
    ASSERT_TRUE(device.write(payload, garbage, sizeof(garbage)));
  }

  device.fail_writes(true);
  Rig blocked{512, RING_RECORD, CAPACITY, &device};

  EXPECT_FALSE(blocked.ring.is_initialised());
  uint64_t value = 1;
  EXPECT_FALSE(blocked.ring.append(value));
}

TEST_F(FramRingGeometryTest, ScansAcrossSeveralBatches) {
  static const uint16_t CAPACITY = 20;
  Rig rig{512, RING_RECORD, CAPACITY};

  for (uint64_t value = 1; value <= 12; value++) {
    ASSERT_TRUE(rig.ring.append(value));
  }

  // An intact ring takes the binary search, so the sweep only runs once something forces the fallback.
  uint32_t zero = 0;
  ASSERT_TRUE(rig.device->write(SLOT_OFFSET, reinterpret_cast<const uint8_t *>(&zero), sizeof(zero)));

  auto restarted = this->remount(rig, RING_RECORD, CAPACITY);

  EXPECT_TRUE(restarted->ring.used_sweep());
  EXPECT_EQ(restarted->store.format_count(), 1u);
  EXPECT_EQ(restarted->ring.count(), 11u);
  uint64_t read = 0;
  ASSERT_TRUE(restarted->ring.peek(0, read));
  EXPECT_EQ(read, 12u);
  ASSERT_TRUE(restarted->ring.peek(10, read));
  EXPECT_EQ(read, 2u);
}

// Past MAX_TORN_RECORDS there is no head to append after, and records left in place would out-rank every
// later append: the ring has to start over for an append to survive a restart at all.
TEST_F(FramRingGeometryTest, WipesItselfWhenTooManyRecordsAreDamaged) {
  static const uint16_t CAPACITY = 8;
  Rig rig{512, RING_RECORD, CAPACITY};

  for (uint64_t value = 1; value <= CAPACITY; value++) {
    ASSERT_TRUE(rig.ring.append(value));
  }
  for (uint16_t index = 0; index < CAPACITY; index++) {
    this->damage_record(rig, index);
  }

  auto wiped = this->remount(rig, RING_RECORD, CAPACITY);
  EXPECT_TRUE(wiped->ring.is_initialised());
  EXPECT_EQ(wiped->ring.count(), 0u);

  uint64_t value = 77;
  ASSERT_TRUE(wiped->ring.append(value));

  auto restarted = this->remount(rig, RING_RECORD, CAPACITY);
  EXPECT_EQ(restarted->ring.count(), 1u);
  ASSERT_TRUE(restarted->ring.peek(0, value));
  EXPECT_EQ(value, 77u);
}

// Records longer than the scan buffer make the scan read headers one at a time, which is also the only
// geometry where a payload read failure is reached through validate_entry_ rather than the scan itself.
TEST_F(FramRingGeometryTest, KeepsRecordsWhenAPayloadCannotBeRead) {
  static const uint16_t RECORD = 128;
  static const uint16_t CAPACITY = 3;
  static const uint16_t HEAD_PAYLOAD = SLOT_OFFSET + 2 * (FramRing::HEADER_SIZE + RECORD) + FramRing::HEADER_SIZE;

  FaultyBackend device{512};
  {
    Rig rig{512, RECORD, CAPACITY, &device};
    for (uint8_t value = 1; value <= CAPACITY; value++) {
      uint8_t record[RECORD];
      std::memset(record, value, sizeof(record));
      ASSERT_TRUE(rig.ring.append(record, sizeof(record)));
    }
  }

  device.fail_reads_covering(HEAD_PAYLOAD);
  {
    Rig blocked{512, RECORD, CAPACITY, &device};
    EXPECT_FALSE(blocked.ring.is_initialised());
    uint8_t record[RECORD] = {};
    EXPECT_FALSE(blocked.ring.append(record, sizeof(record)));
  }

  device.stop_failing();
  Rig healthy{512, RECORD, CAPACITY, &device};
  EXPECT_EQ(healthy.ring.count(), CAPACITY);
  uint8_t read[RECORD] = {};
  ASSERT_TRUE(healthy.ring.peek(0, read, sizeof(read)));
  EXPECT_EQ(read[0], CAPACITY);
}

TEST_F(FramStoreTest, RingPeekFailsWhenAPayloadCannotBeRead) {
  auto mounted = this->mount();
  for (uint64_t value = 1; value <= 3; value++) {
    ASSERT_TRUE(mounted->ring.append(value));
  }

  this->backend_.fail_reads_covering(ring_payload(1));
  uint64_t read = 0;
  EXPECT_FALSE(mounted->ring.peek(1, read));
  ASSERT_TRUE(mounted->ring.peek(0, read));
  EXPECT_EQ(read, 3u);
}

// A header the binary search cannot read says nothing about where the head is: the ring stays shut
// rather than guess, and opens normally once the part answers again.
TEST_F(FramRingGeometryTest, ASearchThatCannotReadLeavesTheRingUnusable) {
  static const uint16_t CAPACITY = 8;
  FaultyBackend device{512};
  {
    Rig rig{512, RING_RECORD, CAPACITY, &device};
    for (uint64_t value = 1; value <= CAPACITY + 3; value++) {
      ASSERT_TRUE(rig.ring.append(value));
    }
  }

  // Wrapped, the search starts between index 0 and the last one: index 3 is its first probe.
  device.fail_reads_covering(SLOT_OFFSET + 3 * (FramRing::HEADER_SIZE + RING_RECORD));
  {
    Rig blocked{512, RING_RECORD, CAPACITY, &device};
    EXPECT_FALSE(blocked.ring.is_initialised());
    EXPECT_FALSE(blocked.ring.used_sweep());
    uint64_t value = 1;
    EXPECT_FALSE(blocked.ring.append(value));
  }

  device.stop_failing();
  Rig healthy{512, RING_RECORD, CAPACITY, &device};
  EXPECT_EQ(healthy.ring.total_appended(), CAPACITY + 3u);
}

// The sweep reads headers one at a time for records this long; one it cannot read shuts the ring.
TEST_F(FramRingGeometryTest, ASweepThatCannotReadLeavesTheRingUnusable) {
  static const uint16_t RECORD = 128;
  static const uint16_t CAPACITY = 3;
  static const uint16_t ENTRY = FramRing::HEADER_SIZE + RECORD;

  FaultyBackend device{512};
  {
    Rig rig{512, RECORD, CAPACITY, &device};
    for (uint8_t value = 1; value <= CAPACITY; value++) {
      uint8_t record[RECORD];
      std::memset(record, value, sizeof(record));
      ASSERT_TRUE(rig.ring.append(record, sizeof(record)));
    }
  }

  // A zeroed first sequence sends the ring to the sweep; the header after it does not answer.
  uint32_t zero = 0;
  ASSERT_TRUE(device.write(SLOT_OFFSET, reinterpret_cast<const uint8_t *>(&zero), sizeof(zero)));
  device.fail_reads_covering(SLOT_OFFSET + ENTRY);

  Rig blocked{512, RECORD, CAPACITY, &device};
  EXPECT_TRUE(blocked.ring.used_sweep());
  EXPECT_FALSE(blocked.ring.is_initialised());
  uint8_t record[RECORD] = {};
  EXPECT_FALSE(blocked.ring.append(record, sizeof(record)));
}

}  // namespace esphome::fram_store::testing
