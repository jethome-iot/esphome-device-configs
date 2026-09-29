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

// Upstream checks is_internal() when it queues an entity for an API client, and reads the name
// and key only when it encodes it, later. A slot renamed in between reached Home Assistant as a
// climate nobody made, so hiding flips the internal bit and nothing else.
TEST_F(HubTest, HidingKeepsTheNameAndTheKey) {
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  const uint32_t key = entity->get_object_id_hash();
  const char *name = entity->get_name().c_str();

  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  EXPECT_TRUE(entity->is_internal());
  EXPECT_TRUE(entity->is_free());
  EXPECT_TRUE(entity->is_named());
  EXPECT_EQ(name, entity->get_name().c_str()) << "the same bytes";
  EXPECT_STREQ("Boiler", entity->get_name().c_str());
  EXPECT_EQ(key, entity->get_object_id_hash());
  EXPECT_EQ(nullptr, App.get_climate_by_key(key)) << "internal: the API cannot address it";
  EXPECT_EQ(entity, App.get_climate_by_key(key, true));
}

// Its last word, while clients still list it: stopped. Then it drops out of every listing.
TEST_F(HubTest, AStopPublishesTheStoppedStateBeforeHiding) {
  struct Seen {
    bool internal;
    climate::ClimateMode mode;
    climate::ClimateAction action;
    float temperature;
  };
  // Callbacks cannot be removed and the slot outlives the test: record only while armed.
  static std::vector<Seen> seen;
  static bool armed = false;
  static bool subscribed = false;
  entities().room.publish_state(18.f);
  this->create(draft("Boiler"));
  HubClimate *entity = hub().entity_of("boiler");
  if (!subscribed) {
    entity->add_on_state_callback([](climate::Climate &c) {
      if (armed)
        seen.push_back({c.is_internal(), c.mode, c.action, c.current_temperature});
    });
    subscribed = true;
  }
  seen.clear();
  armed = true;
  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  armed = false;

  ASSERT_EQ(1u, seen.size());
  EXPECT_FALSE(seen[0].internal) << "published while still listed";
  EXPECT_EQ(climate::CLIMATE_MODE_OFF, seen[0].mode);
  EXPECT_EQ(climate::CLIMATE_ACTION_OFF, seen[0].action);
  EXPECT_TRUE(std::isnan(seen[0].temperature)) << "a stopped thermostat reads nothing";
  EXPECT_TRUE(entity->is_internal());
}

// A thermostat that comes back under a name a hidden slot still carries gets that slot, and with
// it the key Home Assistant knew, rather than the one first in the queue.
TEST_F(HubTest, TheSameNameComesBackOnTheSameSlot) {
  this->create(draft("Boiler", "relay_1"));
  this->create(draft("Kettle", "relay_2"));
  HubClimate *boiler = hub().entity_of("boiler");
  const uint32_t key = boiler->get_object_id_hash();

  ASSERT_TRUE(hub().remove("boiler").ok);
  this->create(draft("Boiler", "relay_1"));
  EXPECT_EQ(boiler, hub().entity_of("boiler")) << "not slot 2, first in the queue";
  EXPECT_EQ(key, boiler->get_object_id_hash());

  // Stopped and started again with another slot first in the queue.
  ASSERT_TRUE(hub().set_enabled("boiler", false).ok);
  this->create(draft("Porch", "relay_3"));
  ASSERT_TRUE(hub().set_enabled("boiler", true).ok);
  EXPECT_EQ(boiler, hub().entity_of("boiler"));

  // The same object id is the same entity to Home Assistant, whatever the spelling.
  ASSERT_TRUE(hub().remove("boiler").ok);
  this->create(draft("BOILER", "relay_1"));
  EXPECT_EQ(boiler, hub().entity_of("boiler"));
  EXPECT_STREQ("BOILER", boiler->get_name().c_str());
}

// A hidden slot keeps its name only while nobody else wants it: the web server answers the first
// climate by that name, so the new owner would never be reached behind it.
TEST_F(HubTest, ARenameOntoAHiddenSlotsNameParksThatSlot) {
  this->create(draft("Room 1", "relay_1"));
  this->create(draft("Kettle", "relay_2"));
  HubClimate *hidden = hub().entity_of("room-1");
  HubClimate *kettle = hub().entity_of("kettle");
  ASSERT_TRUE(hub().remove("room-1").ok);
  ASSERT_STREQ("Room 1", hidden->get_name().c_str());

  ASSERT_TRUE(hub().update("kettle", draft("Room_1", "relay_2")).ok);
  EXPECT_EQ(kettle, hub().entity_of("kettle")) << "a rename stays on its own slot";
  EXPECT_STREQ(FREE_SLOT_NAME, hidden->get_name().c_str()) << "same object id: room_1";
  EXPECT_TRUE(hidden->is_internal());
  EXPECT_FALSE(hidden->is_named());
  EXPECT_EQ(1u, hidden->get_traits().get_supported_modes().size()) << "a parked slot offers only off";
  EXPECT_EQ(kettle, web_server_match("Room_1"));
  EXPECT_EQ(nullptr, web_server_match("Room 1"));
}

// The same when a thermostat starts under a name that differs from the hidden one only in case
// and spacing: that is another object id, so another slot, and the hidden one gives way.
TEST_F(HubTest, AStartOntoAHiddenSlotsNameParksThatSlot) {
  this->create(draft("Room 1", "relay_1"));
  HubClimate *hidden = hub().entity_of("room-1");
  ASSERT_TRUE(hub().remove("room-1").ok);

  this->create(draft("room  1", "relay_1"));
  HubClimate *started = hub().entity_of("room-1");
  EXPECT_NE(hidden, started) << "room__1 is not room_1";
  EXPECT_STREQ(FREE_SLOT_NAME, hidden->get_name().c_str()) << "same name to a person";
  EXPECT_STREQ("room  1", started->get_name().c_str());
}

// The placeholder marks a slot nobody has used since boot. Coming and going, renames and a
// take-over never bring it back: only a collision above does.
TEST_F(HubTest, ThePlaceholderIsOnlyForSlotsNeverUsed) {
  std::set<const HubClimate *> used;
  auto check = [&used](const char *step) {
    for (const std::string &id : {"a", "b", "c", "d"}) {
      if (hub().entity_of(id) != nullptr)
        used.insert(hub().entity_of(id));
    }
    for (size_t i = 0; i < hub().slot_count(); i++) {
      const HubClimate *slot = hub().slot_entity(i);
      EXPECT_EQ(used.count(slot) != 0, slot->is_named()) << step << ", slot " << i;
      EXPECT_EQ(used.count(slot) == 0, std::string(FREE_SLOT_NAME) == slot->get_name().c_str())
          << step << ", slot " << i;
    }
  };
  check("setup");
  this->create(draft("A", "relay_1"));
  check("create");
  ASSERT_TRUE(hub().update("a", draft("Alpha", "relay_1")).ok);
  check("rename");
  ASSERT_TRUE(hub().set_enabled("a", false).ok);
  check("disable");
  this->create(draft("B", "relay_2"));
  ClimateConfig c = draft("C", "relay_2");
  c.enabled = false;
  this->create(c);
  check("create more");
  ASSERT_TRUE(hub().set_enabled("c", true, true).ok);
  check("take over");
  ASSERT_TRUE(hub().set_enabled("a", true).ok);
  check("enable");
  ASSERT_TRUE(hub().remove("a").ok);
  this->create(draft("D", "relay_1"));
  check("remove and create");
  this->reboot();
  used.clear();
  check("reboot");
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
