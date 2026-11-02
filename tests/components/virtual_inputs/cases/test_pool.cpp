// The pool: its entities, the boot from the file, and saving and deleting a slot.
#include "common.h"

namespace esphome::virtual_inputs::testing {

static const char *const FILE_NAME = "virtual_inputs.json";

static std::string object_id(const EntityBase &entity) {
  char buf[OBJECT_ID_MAX_LEN];
  return std::string(entity.get_object_id_to(buf));
}

TEST_F(Pool, EverySlotIsRegisteredOnceUnderItsFixedNameAndHidden) {
  boot();
  ASSERT_EQ(pool().pool_size(), 4u);
  EXPECT_EQ(pool().max_inputs(), 4u);
  for (size_t slot = 0; slot < 4; slot++) {
    const VirtualInput &entity = pool().entity(slot);
    EXPECT_EQ(entity.get_name().str(), "Virtual input " + std::to_string(slot + 1));
    EXPECT_EQ(object_id(entity), "virtual_input_" + std::to_string(slot + 1));
    EXPECT_TRUE(entity.is_internal());
    EXPECT_FALSE(entity.has_state());
    EXPECT_EQ(pool().sensor(slot), &entity);
    EXPECT_TRUE(pool().is_virtual_input(&entity));
    EXPECT_FALSE(pool().configured(slot));
    // Never inverted, whatever a record says: entity_config knows them as labels only.
    EXPECT_TRUE(store().inputs.is_label_only(&entity));
  }
  EXPECT_EQ(pool().sensor(4), nullptr);
  EXPECT_FALSE(pool().configured(4));
  EXPECT_FALSE(pool().config(4).configured());
  EXPECT_FALSE(pool().is_virtual_input(&e.in1));
  EXPECT_EQ(pool().slot_for_new(), 0);
  // A second boot reuses the pool: App has no room for another.
  const size_t sensors = App.get_binary_sensors().size();
  reboot();
  EXPECT_EQ(App.get_binary_sensors().size(), sensors);
  EXPECT_EQ(pool().pool_size(), 4u);
}

TEST_F(Pool, TheSourcesAreTheVisibleInputsThatAreNotVirtual) {
  boot();
  const std::vector<binary_sensor::BinarySensor *> expected = {&e.in1, &e.in2, &e.in3};
  EXPECT_EQ(pool().sources(), expected);
  EXPECT_EQ(pool().check_source("input_1"), SourceCheck::OK);
  EXPECT_EQ(pool().check_source("hidden"), SourceCheck::MISSING);
  EXPECT_EQ(pool().check_source("relay_1"), SourceCheck::MISSING);  // a switch is no input
  EXPECT_EQ(pool().check_source("no_such_input"), SourceCheck::MISSING);
  EXPECT_EQ(pool().check_source(""), SourceCheck::MISSING);
  EXPECT_EQ(pool().check_source("virtual_input_2"), SourceCheck::VIRTUAL);
  ASSERT_TRUE(pool().save(1, follow_config("input_2")));
  // Visible now, and still no source.
  EXPECT_EQ(pool().check_source("virtual_input_2"), SourceCheck::VIRTUAL);
  EXPECT_EQ(pool().sources(), expected);
}

TEST_F(Pool, ABootShowsTheStoredSlotsAndTakesTheirSourcesFirstLevel) {
  write(FILE_NAME, R"({"version":1,"records":[{"slot":2,"source":"input_1","on_delay":0,"off_delay":60},)"
                   R"({"slot":4,"source":"input_2","on_delay":10,"off_delay":0}]})");
  boot(true, true);
  EXPECT_TRUE(pool().entity(0).is_internal());
  EXPECT_FALSE(pool().configured(0));
  EXPECT_FALSE(pool().entity(1).is_internal());
  EXPECT_TRUE(pool().entity(2).is_internal());
  EXPECT_FALSE(pool().entity(3).is_internal());
  EXPECT_EQ(pool().config(1), follow_config("input_1", 0, 60));
  EXPECT_EQ(pool().config(3), follow_config("input_2", 10, 0));
  // No on delay: on as soon as its source reported on.
  ASSERT_TRUE(pool().entity(1).has_state());
  EXPECT_TRUE(pool().entity(1).state);
  // An on delay counts from boot: off until it has passed.
  ASSERT_TRUE(pool().entity(3).has_state());
  EXPECT_FALSE(pool().entity(3).state);
  pool().advance(9999);
  EXPECT_FALSE(pool().entity(3).state);
  pool().advance(1);
  EXPECT_TRUE(pool().entity(3).state);
  EXPECT_EQ(pool().problem(1), "");
  EXPECT_EQ(pool().problem(0), "");  // free
  EXPECT_EQ(pool().slot_for_new(), 0);
}

TEST_F(Pool, ASlotHasNoStateUntilItsSourceHasOne) {
  write(FILE_NAME, R"({"records":[{"slot":1,"source":"input_1","off_delay":5}]})");
  store().keeper.setup();
  pool().setup();
  EXPECT_FALSE(pool().entity(0).is_internal());
  EXPECT_FALSE(pool().entity(0).has_state());
  EXPECT_EQ(pool().problem(0), "Waiting for 'input_1' to report");
  e.in1.publish_state(false);
  ASSERT_TRUE(pool().entity(0).has_state());
  EXPECT_FALSE(pool().entity(0).state);
  EXPECT_EQ(pool().problem(0), "");
}

TEST_F(Pool, ASlotWhoseSourceIsNotAnInputStaysOffAndSaysWhy) {
  write(FILE_NAME, R"({"records":[{"slot":1,"source":"input_9"},{"slot":2,"source":"virtual_input_1"},)"
                   R"({"slot":3,"source":"hidden"}]})");
  boot(true, true);
  for (size_t slot = 0; slot < 3; slot++) {
    EXPECT_FALSE(pool().entity(slot).is_internal()) << slot;
    ASSERT_TRUE(pool().entity(slot).has_state()) << slot;
    EXPECT_FALSE(pool().entity(slot).state) << slot;
  }
  EXPECT_EQ(pool().problem(0), "Source 'input_9' not found");
  EXPECT_EQ(pool().problem(1), "Source 'virtual_input_1' is a virtual input");
  EXPECT_EQ(pool().problem(2), "Source 'hidden' not found");
  EXPECT_TRUE(log().has_warning("Virtual input 1: source 'input_9' is not an input here, staying off"));
}

TEST_F(Pool, AFileThatDidNotLoadIsLeftForAPersonAndNothingIsSaved) {
  write(FILE_NAME, R"({"records":[{"slot":1,"source":"input_1"})");
  boot(true);
  EXPECT_FALSE(pool().file_loaded());
  EXPECT_TRUE(pool().can_save());
  EXPECT_FALSE(pool().configured(0));
  EXPECT_TRUE(log().has_error("virtual_inputs.json did not load"));
  EXPECT_FALSE(pool().save(0, follow_config("input_1")));
  EXPECT_FALSE(pool().configured(0));
  EXPECT_EQ(read(FILE_NAME), R"({"records":[{"slot":1,"source":"input_1"})");
  pool().dump_config();  // names the file as one that did not load
}

TEST_F(Pool, ADeleteIsRefusedWhenTheStorageCannotKeepIt) {
  write(FILE_NAME, R"({"records":[{"slot":1,"source":"input_1"}]})");
  boot(true);
  ASSERT_TRUE(pool().configured(0));
  pool().set_keeper(&store().failed);
  EXPECT_FALSE(pool().remove(0));
  EXPECT_TRUE(pool().configured(0));
  EXPECT_FALSE(pool().entity(0).is_internal());
}

TEST_F(Pool, AFullPoolHasNoFreeSlot) {
  boot();
  for (size_t slot = 0; slot < 4; slot++)
    ASSERT_TRUE(pool().save(slot, follow_config("input_1")));
  EXPECT_EQ(pool().slot_for_new(), -1);
  ASSERT_TRUE(pool().remove(2));
  EXPECT_EQ(pool().slot_for_new(), 2);
}

TEST_F(Pool, APoolTheEntityTableHasNoRoomForRegistersNothingAndSaysSo) {
  boot();  // the cases' own pool fills the table test.yaml sized
  static VirtualInputs *extra = [] {
    auto *inputs = new VirtualInputs();  // never freed: an entity would keep a pointer into it
    inputs->set_max_inputs(2);
    inputs->set_keeper(&store().keeper);
    return inputs;
  }();
  const size_t sensors = App.get_binary_sensors().size();
  extra->setup();
  EXPECT_TRUE(log().has_error("No room in the entity table for virtual input 1"));
  EXPECT_EQ(App.get_binary_sensors().size(), sensors);
  EXPECT_EQ(extra->slot_for_new(), -1);
  EXPECT_FALSE(extra->configured(0));
  EXPECT_EQ(extra->sensor(0), nullptr);
  EXPECT_FALSE(extra->save(0, follow_config("input_1")));
}

TEST_F(Pool, WithoutStorageThereAreNoSlotsAndNothingIsSaved) {
  write(FILE_NAME, R"({"records":[{"slot":1,"source":"input_1"}]})");
  pool().set_keeper(&store().failed);
  boot(true);
  EXPECT_FALSE(pool().can_save());
  EXPECT_TRUE(pool().file_loaded());
  EXPECT_FALSE(pool().configured(0));
  EXPECT_TRUE(log().has_warning("Storage unavailable: no virtual inputs this boot"));
  EXPECT_FALSE(pool().save(0, follow_config("input_1")));
  pool().set_keeper(nullptr);
  EXPECT_FALSE(pool().can_save());
}

TEST_F(Pool, ASaveShowsAFreeSlotWritesTheFileAndAsksHomeAssistantToReconnect) {
  boot(true);
  ASSERT_TRUE(pool().save(2, follow_config("input_1", 0, 30)));
  EXPECT_TRUE(pool().configured(2));
  EXPECT_FALSE(pool().entity(2).is_internal());
  ASSERT_TRUE(pool().entity(2).has_state());
  EXPECT_TRUE(pool().entity(2).state);  // its source is on, and there is no on delay
  EXPECT_EQ(read(FILE_NAME), R"({"version":1,"records":[{"slot":3,"source":"input_1","on_delay":0,"off_delay":30}]})");
  EXPECT_EQ(pool().slot_for_new(), 0);
  EXPECT_EQ(pool().resyncs, 0);
  pass_resync_delay();
  EXPECT_EQ(pool().resyncs, 1);

  // It survives a reboot.
  reboot(true);
  EXPECT_EQ(pool().config(2), follow_config("input_1", 0, 30));
  EXPECT_TRUE(pool().entity(2).state);
}

TEST_F(Pool, ASaveIsRefusedWithNothingChangedForWhatCannotBeKept) {
  boot();
  EXPECT_FALSE(pool().save(4, follow_config("input_1")));  // past the pool
  EXPECT_FALSE(pool().save(0, follow_config("")));
  EXPECT_FALSE(pool().save(0, follow_config("input_9")));
  EXPECT_FALSE(pool().save(0, follow_config("hidden")));
  EXPECT_FALSE(pool().save(0, follow_config("virtual_input_2")));
  EXPECT_FALSE(pool().save(0, follow_config("virtual_input_1")));
  EXPECT_FALSE(pool().save(0, follow_config("input_1", MAX_DELAY_S + 1, 0)));
  EXPECT_FALSE(pool().save(0, follow_config("input_1", 0, MAX_DELAY_S + 1)));
  EXPECT_TRUE(pool().save(0, follow_config("input_1", MAX_DELAY_S, MAX_DELAY_S)));
  for (size_t slot = 1; slot < 4; slot++) {
    EXPECT_FALSE(pool().configured(slot));
    EXPECT_TRUE(pool().entity(slot).is_internal());
  }
}

TEST_F(Pool, AWriteThatFailsChangesNothing) {
  boot(true);
  ASSERT_TRUE(pool().save(0, follow_config("input_1")));
  const std::string before = read(FILE_NAME);
  // The folder takes no new file, so the write into its sibling .tmp fails.
  chmod(config_dir().c_str(), 0555);
  EXPECT_FALSE(pool().save(1, follow_config("input_2")));
  EXPECT_FALSE(pool().save(0, follow_config("input_2")));
  EXPECT_FALSE(pool().remove(0));
  chmod(config_dir().c_str(), 0755);
  EXPECT_FALSE(pool().configured(1));
  EXPECT_TRUE(pool().entity(1).is_internal());
  EXPECT_EQ(pool().config(0), follow_config("input_1"));
  EXPECT_FALSE(pool().entity(0).is_internal());
  EXPECT_EQ(read(FILE_NAME), before);
  // What the file object holds is still what is in force: the next write carries no stray slot.
  ASSERT_TRUE(pool().save(2, follow_config("input_1")));
  EXPECT_EQ(read(FILE_NAME), R"({"version":1,"records":[{"slot":1,"source":"input_1","on_delay":0,"off_delay":0},)"
                             R"({"slot":3,"source":"input_1","on_delay":0,"off_delay":0}]})");
}

TEST_F(Pool, TheSameConfigAgainWritesNothing) {
  boot();
  ASSERT_TRUE(pool().save(0, follow_config("input_1", 1, 2)));
  remove((config_dir() + "/" + FILE_NAME).c_str());
  EXPECT_TRUE(pool().save(0, follow_config("input_1", 1, 2)));
  EXPECT_FALSE(exists(FILE_NAME));
}

TEST_F(Pool, AnEditTakesTheNewSourceAndCountsItsDelaysFromTheSave) {
  boot(false, true);
  ASSERT_TRUE(pool().save(0, follow_config("input_1", 0, 100)));
  EXPECT_FALSE(pool().entity(0).state);
  pass_resync_delay();
  EXPECT_EQ(pool().resyncs, 1);

  // To input 2, which is on, behind a 5 s on delay: off until then, counted from the save.
  pool().advance(1000);
  ASSERT_TRUE(pool().save(0, follow_config("input_2", 5, 100)));
  EXPECT_FALSE(pool().entity(0).state);
  pool().advance(4999);
  EXPECT_FALSE(pool().entity(0).state);
  pool().advance(1);
  EXPECT_TRUE(pool().entity(0).state);
  // Input 1 drives it no more; input 2 does.
  e.in1.publish_state(true);
  e.in1.publish_state(false);
  EXPECT_TRUE(pool().entity(0).state);
  e.in2.publish_state(false);
  pool().advance(99999);
  EXPECT_TRUE(pool().entity(0).state);
  // A shorter off delay while the count runs: counted again, under the new one, from the save.
  ASSERT_TRUE(pool().save(0, follow_config("input_2", 5, 3)));
  EXPECT_TRUE(pool().entity(0).state);
  pool().advance(2999);
  EXPECT_TRUE(pool().entity(0).state);
  pool().advance(1);
  EXPECT_FALSE(pool().entity(0).state);
  // An edit is no new entity: Home Assistant has nothing to list again.
  pass_resync_delay();
  EXPECT_EQ(pool().resyncs, 1);
}

TEST_F(Pool, ADeleteHidesTheSlotWithNoStateAndFreesIt) {
  boot(true);
  ASSERT_TRUE(pool().save(0, follow_config("input_1")));
  ASSERT_TRUE(pool().save(1, follow_config("input_1")));
  pass_resync_delay();
  EXPECT_EQ(pool().resyncs, 1);
  ASSERT_TRUE(pool().remove(0));
  EXPECT_FALSE(pool().configured(0));
  EXPECT_TRUE(pool().entity(0).is_internal());
  EXPECT_FALSE(pool().entity(0).has_state());
  EXPECT_EQ(pool().slot_for_new(), 0);
  EXPECT_EQ(read(FILE_NAME), R"({"version":1,"records":[{"slot":2,"source":"input_1","on_delay":0,"off_delay":0}]})");
  pass_resync_delay();
  EXPECT_EQ(pool().resyncs, 2);
  // Its source drives it no more.
  e.in1.publish_state(false);
  e.in1.publish_state(true);
  EXPECT_FALSE(pool().entity(0).has_state());
  EXPECT_FALSE(pool().remove(0));  // free already
  EXPECT_FALSE(pool().remove(9));

  // A new one in the slot starts from its source's level, as at boot.
  ASSERT_TRUE(pool().save(0, follow_config("input_2")));
  ASSERT_TRUE(pool().entity(0).has_state());
  EXPECT_FALSE(pool().entity(0).state);
}

TEST_F(Pool, BurstsOfChangesCostOneReconnect) {
  boot();
  ASSERT_TRUE(pool().save(0, follow_config("input_1")));
  ASSERT_TRUE(pool().save(1, follow_config("input_1")));
  ASSERT_TRUE(pool().remove(0));
  pass_resync_delay();
  EXPECT_EQ(pool().resyncs, 1);
}

TEST_F(Pool, OneSubscriptionPerSourceServesEverySlotOnIt) {
  boot();
  ASSERT_TRUE(pool().save(0, follow_config("input_1", 0, 10)));
  ASSERT_TRUE(pool().save(2, follow_config("input_2")));
  const size_t subscribed = pool().subscriptions();
  EXPECT_LE(subscribed, 3u);  // one per input at most, over every case so far
  ASSERT_TRUE(pool().save(1, follow_config("input_1", 2, 0)));
  EXPECT_EQ(pool().subscriptions(), subscribed);
  e.in1.publish_state(true);
  EXPECT_TRUE(pool().entity(0).state);
  EXPECT_FALSE(pool().entity(1).state);
  EXPECT_FALSE(pool().entity(2).state);
  pool().advance(2000);
  EXPECT_TRUE(pool().entity(1).state);
  e.in1.publish_state(false);
  EXPECT_TRUE(pool().entity(0).state);
  EXPECT_FALSE(pool().entity(1).state);
  pool().advance(10000);
  EXPECT_FALSE(pool().entity(0).state);
}

TEST_F(Pool, ASmallerPoolSkipsTheSlotsPastItUntilAWriteDropsThem) {
  write(FILE_NAME, R"({"records":[{"slot":1,"source":"input_1"},{"slot":6,"source":"input_2"}]})");
  boot();
  EXPECT_TRUE(pool().configured(0));
  EXPECT_TRUE(log().has_warning("Slot 6 is past max_inputs, skipping"));
  EXPECT_EQ(read(FILE_NAME), R"({"records":[{"slot":1,"source":"input_1"},{"slot":6,"source":"input_2"}]})");
  ASSERT_TRUE(pool().save(1, follow_config("input_2")));
  EXPECT_EQ(read(FILE_NAME), R"({"version":1,"records":[{"slot":1,"source":"input_1","on_delay":0,"off_delay":0},)"
                             R"({"slot":2,"source":"input_2","on_delay":0,"off_delay":0}]})");
}

TEST_F(Pool, TheConfigDumpNamesEverySlotInUse) {
  write(FILE_NAME, R"({"records":[{"slot":2,"source":"input_1","on_delay":3,"off_delay":4}]})");
  boot();
  pool().dump_config();  // reaches the log only; the case is that it runs over a booted pool
  pool().set_keeper(nullptr);
  pool().dump_config();
}

}  // namespace esphome::virtual_inputs::testing
