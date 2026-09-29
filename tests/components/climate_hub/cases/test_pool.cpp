// The pool of climate entities and the boot load. Upstream registers entities only at setup and
// never removes or renames one from outside, so the hub registers every slot up front, hidden,
// and hands them out and back; the boot load decides which files get one.
#include "common.h"

namespace esphome::climate_hub::testing {

namespace {

const char *const DOC = R"({"version":1,"id":"%s","name":"%s","enabled":%s,"kind":"bang_bang",)"
                        R"("sensor_id":"room","heat":{"relay_id":"%s"},"mode":"heat","setpoint":21})";

std::string doc(const char *id, const char *name, const char *relay = "relay_1", bool enabled = true) {
  char buf[512];
  snprintf(buf, sizeof(buf), DOC, id, name, enabled ? "true" : "false", relay);
  return buf;
}

}  // namespace

TEST_F(HubTest, EverySlotIsRegisteredAtSetupAndHiddenUnderThePlaceholder) {
  ASSERT_EQ(4u, hub().slot_count());
  const auto &climates = App.get_climates();
  EXPECT_EQ(climates.capacity(), climates.size()) << "the pool and the YAML climate fill what codegen reserved";
  for (size_t i = 0; i < hub().slot_count(); i++) {
    HubClimate *slot = hub().slot_entity(i);
    EXPECT_NE(climates.end(), std::find(climates.begin(), climates.end(), slot)) << i;
    EXPECT_TRUE(slot->is_internal()) << i;
    EXPECT_STREQ(FREE_SLOT_NAME, slot->get_name().c_str()) << i;
  }
}

// Hash 0 at registration: the object id hash comes from the name, as codegen's does, so a record
// keyed by fnv1_hash(object id) finds the thermostat.
TEST_F(HubTest, AStartedSlotIsVisibleWithTheNameDerivedHash) {
  this->create(draft("Living Room"));
  HubClimate *entity = hub().entity_of("living-room");
  EXPECT_FALSE(entity->is_internal());
  EXPECT_EQ(fnv1_hash("living_room"), entity->get_object_id_hash());
  EXPECT_EQ(entity, App.get_climate_by_key(fnv1_hash("living_room")));
}

// The entity keeps a pointer to its name. A rename writes the other buffer, so a reader that
// still holds the previous pointer reads the old name whole.
TEST_F(HubTest, ARenameSwapsTheNameBuffer) {
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  const char *before = entity->get_name().c_str();

  ASSERT_TRUE(hub().update("boiler", draft("Hot Water")).ok);
  const char *after = entity->get_name().c_str();
  EXPECT_NE(before, after);
  EXPECT_STREQ("Boiler", before);
  EXPECT_STREQ("Hot Water", after);
  EXPECT_EQ(fnv1_hash("hot_water"), entity->get_object_id_hash());
}

// A slot handed back goes to the end of the queue: a web server call resolved just before it was
// freed is unlikely to find it taken again.
TEST_F(HubTest, AStoppedSlotIsHiddenAndRecycledLast) {
  this->create(draft("A", "relay_1"));
  this->create(draft("B", "relay_2"));
  HubClimate *a = hub().entity_of("a");
  ASSERT_EQ(hub().slot_entity(0), a);
  ASSERT_EQ(hub().slot_entity(1), hub().entity_of("b"));

  ASSERT_TRUE(hub().remove("a").ok);
  EXPECT_TRUE(a->is_internal());
  EXPECT_EQ(3u, hub().free_count());

  this->create(draft("C", "relay_1"));
  EXPECT_EQ(hub().slot_entity(2), hub().entity_of("c")) << "first in, first out";
  this->create(draft("D", "relay_3"));
  ClimateConfig e = draft("E", "relay_1");
  e.enabled = false;
  this->create(e);
  ASSERT_TRUE(hub().set_enabled("c", false).ok);
  ASSERT_TRUE(hub().set_enabled("e", true).ok);
  EXPECT_EQ(a, hub().entity_of("e")) << "the freed slot comes round again";
}

// However many times thermostats come and go, the entity table does not grow: nothing is
// registered after setup.
TEST_F(HubTest, CreateAndDeleteCyclesRegisterNothing) {
  const size_t registered = App.get_climates().size();
  for (int round = 0; round < 10; round++) {
    this->create(draft("Boiler"));
    ASSERT_TRUE(hub().remove("boiler").ok);
  }
  EXPECT_EQ(registered, App.get_climates().size());
  EXPECT_EQ(4u, hub().free_count());
}

// A call the web server resolved before the slot was freed must not steer anything.
TEST_F(HubTest, AFreeSlotIgnoresControl) {
  HubClimate *slot = hub().slot_entity(0);
  ASSERT_TRUE(slot->is_free());
  auto call = slot->make_call();
  call.set_mode(climate::CLIMATE_MODE_OFF);
  call.set_target_temperature(25.f);
  call.perform();
  EXPECT_EQ(climate::CLIMATE_MODE_OFF, slot->mode);
  EXPECT_TRUE(std::isnan(slot->target_temperature) || slot->target_temperature != 25.f);
}

// The folder is writable by hand and a restored backup lands there too, so boot re-establishes
// what the editor refuses.
TEST_F(HubTest, ADuplicateNameOnFlashIsRenamedAtBoot) {
  write_file(this->file_of("boiler"), doc("boiler", "Boiler", "relay_1", false));
  write_file(this->file_of("kettle"), doc("kettle", "boiler", "relay_2", false));
  this->reboot();

  EXPECT_EQ("Boiler", hub().store().get("boiler")->name) << "the first one by id keeps the name";
  EXPECT_EQ("boiler 2", hub().store().get("kettle")->name);
  EXPECT_NE(std::string::npos, read_file(this->file_of("kettle")).find("\"name\":\"boiler 2\""))
      << "the new name has to survive the next boot too";
}

TEST_F(HubTest, AYamlClimatesNameOnFlashIsRenamedAtBoot) {
  write_file(this->file_of("hall"), doc("hall", "Hall"));
  this->reboot();
  EXPECT_EQ("Hall 2", hub().store().get("hall")->name);
  EXPECT_TRUE(hub().is_running("hall"));
}

TEST_F(HubTest, BootRefusesAFileWhoseIdIsNotItsName) {
  const std::string text = doc("kettle", "Kettle");
  write_file(this->file_of("boiler"), text);
  this->reboot();
  EXPECT_EQ(0u, hub().store().size());
  EXPECT_EQ(text, read_file(this->file_of("boiler"))) << "left byte for byte";
}

TEST_F(HubTest, BootRefusesABrokenDocumentAndLeavesIt) {
  const std::string bad_name = doc("porch", "Up/down");
  const std::string not_json = "{\"id\":";
  write_file(this->file_of("porch"), bad_name);
  write_file(this->file_of("broken"), not_json);
  this->reboot();
  EXPECT_EQ(0u, hub().store().size());
  EXPECT_EQ(bad_name, read_file(this->file_of("porch")));
  EXPECT_EQ(not_json, read_file(this->file_of("broken")));
}

// An id is loaded once: the same document offered twice is refused the second time.
TEST_F(HubTest, BootRefusesADuplicateId) {
  write_file(this->file_of("boiler"), doc("boiler", "Boiler"));
  this->reboot();
  ASSERT_TRUE(hub().is_running("boiler"));
  const std::string text = read_file(this->file_of("boiler"));

  hub().setup();  // the same folder again, without a reset
  EXPECT_EQ(1u, hub().store().size());
  EXPECT_EQ(3u, hub().free_count()) << "no second entity for it either";
  EXPECT_EQ(text, read_file(this->file_of("boiler")));
}

// max_controllers caps the documents as well as the entities: files past it are left alone.
TEST_F(HubTest, BootLoadsAtMostMaxControllersDocuments) {
  const char *ids[] = {"a", "b", "c", "d", "e"};
  const char *names[] = {"A", "B", "C", "D", "E"};
  for (int i = 0; i < 5; i++)
    write_file(this->file_of(ids[i]), doc(ids[i], names[i], "relay_1", false));
  this->reboot();
  EXPECT_EQ(4u, hub().store().size());
  EXPECT_EQ(nullptr, hub().store().get("e")) << "sorted by file name, the last one waits";
  EXPECT_TRUE(file_exists(this->file_of("e")));
}

// Two enabled documents on one relay: the first by id runs, the second waits its turn.
TEST_F(HubTest, BootStartsOneOfTwoOnTheSameRelay) {
  write_file(this->file_of("summer"), doc("summer", "Summer"));
  write_file(this->file_of("winter"), doc("winter", "Winter"));
  this->reboot();
  EXPECT_TRUE(hub().is_running("summer"));
  EXPECT_FALSE(hub().is_running("winter"));
  EXPECT_TRUE(hub().store().get("winter")->enabled) << "still enabled, just not running";
}

// A hub over an unmounted storage fails, registers no entity, and refuses every change; a job
// handed to it runs in place instead of waiting for a loop that never schedules it.
TEST(HubFailure, NoStorageMeansNoPoolAndNoChanges) {
  entities();
  static FakeStorage unmounted;
  unmounted.path = ".storage/none";
  unmounted.mounted = false;
  static TestHub failed;
  failed.set_storage(&unmounted);
  failed.set_max_controllers(4);
  const size_t registered = App.get_climates().size();

  failed.setup();
  EXPECT_TRUE(failed.is_failed());
  EXPECT_EQ(0u, failed.slot_count());
  EXPECT_EQ(registered, App.get_climates().size());

  Result result = failed.create(draft("Boiler"));
  EXPECT_EQ(500, result.code);
  EXPECT_EQ("Thermostat storage is not available", result.error);
  bool ran = false;
  EXPECT_TRUE(failed.run_on_loop([&ran]() {
    ran = true;
    return true;
  }));
  EXPECT_TRUE(ran);
  // The one process-wide hub is what the other cases use; this one only borrowed the pointer.
  global_climate_hub = &hub();
}

TEST_F(HubTest, TheGlobalPointsAtTheHub) { EXPECT_EQ(&hub(), global_climate_hub); }

}  // namespace esphome::climate_hub::testing
