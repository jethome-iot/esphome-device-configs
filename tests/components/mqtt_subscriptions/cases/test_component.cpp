#include "common.h"
#include <cmath>

namespace esphome::mqtt_subscriptions::testing {

using Result = MqttSubscriptions::Result;

static mqtt_config::CrashGuardRecord armed_streak(uint8_t streak) {
  return mqtt_config::CrashGuardRecord{mqtt_config::CRASH_GUARD_MAGIC, streak, 1, {0, 0}};
}

static SlotConfig outdoor() {
  SlotConfig slot = slot_of("Outdoor temperature", "zigbee2mqtt/outdoor");
  slot.json_path = "temperature";
  slot.unit = "°C";
  slot.decimals = 2;
  return slot;
}

class ComponentTest : public SlotsTest {};

TEST_F(ComponentTest, SetsUpBetweenTheMountAndEntitySettings) {
  MqttSubscriptions subs;
  EXPECT_FLOAT_EQ(subs.get_setup_priority(), setup_priority::HARDWARE + 3.0f);
  EXPECT_GT(subs.get_setup_priority(), setup_priority::HARDWARE + 1.0f);   // entity settings apply
  EXPECT_LT(subs.get_setup_priority(), setup_priority::HARDWARE + 10.0f);  // the storage mounts
  EXPECT_EQ(global_mqtt_subscriptions, &subs);
}

TEST_F(ComponentTest, EachEnabledSlotIsAnEntityOfItsKind) {
  SlotConfig disabled = slot_of("Spare", "spare");
  disabled.enabled = false;
  this->plant({outdoor(), slot_of("Garage door", "z2m/garage", SlotKind::BINARY_SENSOR),
               slot_of("Weather", "weather/text", SlotKind::TEXT_SENSOR), disabled});
  const size_t sensors = App.get_sensors().size();
  const size_t binaries = App.get_binary_sensors().size();
  const size_t texts = App.get_text_sensors().size();
  TestSubscriptions &s = this->boot();

  ASSERT_EQ(App.get_sensors().size(), sensors + 1);
  ASSERT_EQ(App.get_binary_sensors().size(), binaries + 1);
  ASSERT_EQ(App.get_text_sensors().size(), texts + 1);
  sensor::Sensor *sensor = s.sensor(0);
  EXPECT_EQ(App.get_sensors()[sensors], sensor);
  EXPECT_EQ(sensor->get_name(), "Outdoor temperature");
  EXPECT_EQ(sensor->get_object_id_hash(), fnv1_hash("outdoor_temperature"));
  EXPECT_EQ(sensor->get_unit_of_measurement_ref(), "°C");
  EXPECT_EQ(sensor->get_accuracy_decimals(), 2);
  EXPECT_EQ(sensor->get_state_class(), sensor::STATE_CLASS_MEASUREMENT);
  EXPECT_FALSE(sensor->is_internal());
  EXPECT_EQ(App.get_binary_sensors()[binaries], s.binary(1));
  EXPECT_EQ(s.binary(1)->get_name(), "Garage door");
  EXPECT_EQ(App.get_text_sensors()[texts], s.text(2));
  EXPECT_EQ(s.text(2)->get_name(), "Weather");
  EXPECT_FALSE(s.active(3));
  EXPECT_EQ(s.state(3), SlotState::OFF);
  EXPECT_FALSE(s.reboot_required());
}

TEST_F(ComponentTest, AUnitlessNumberHasNoUnit) {
  this->plant({slot_of("Count", "c")});
  TestSubscriptions &s = this->boot();
  EXPECT_EQ(s.sensor(0)->get_unit_of_measurement_ref(), "");
  EXPECT_EQ(s.sensor(0)->get_accuracy_decimals(), 1);
}

// The client hands a message to every subscription that matches, so a topic shared by two
// slots is subscribed once and read once.
TEST_F(ComponentTest, OneSubscriptionPerTopicAndOneMessageToEverySlotOnIt) {
  SlotConfig humidity = slot_of("Outdoor humidity", "zigbee2mqtt/outdoor");
  humidity.json_path = "humidity";
  humidity.unit = "%";
  this->plant({outdoor(), humidity, slot_of("Weather", "weather/text", SlotKind::TEXT_SENSOR)});
  TestSubscriptions &s = this->boot();
  ASSERT_EQ(this->client->subscriptions().size(), 2u);
  EXPECT_EQ(this->subscriptions_to("zigbee2mqtt/outdoor"), 1u);
  EXPECT_EQ(this->subscriptions_to("weather/text"), 1u);
  for (const auto &sub : this->client->subscriptions())
    EXPECT_EQ(sub.qos, 0);

  this->deliver("zigbee2mqtt/outdoor", R"({"battery":97,"temperature":21.4,"humidity":48})");
  EXPECT_FLOAT_EQ(s.sensor(0)->state, 21.4f);
  EXPECT_FLOAT_EQ(s.sensor(1)->state, 48.0f);
  EXPECT_FALSE(s.text(2)->has_state());
  this->deliver("weather/text", "Sunny");
  EXPECT_EQ(s.text(2)->state, "Sunny");
}

TEST_F(ComponentTest, SlotsThatDoNotRunAreNotSubscribed) {
  SlotConfig disabled = slot_of("Spare", "spare");
  disabled.enabled = false;
  SlotConfig invalid = slot_of("Bad", "bad/#");
  this->plant({disabled, invalid});
  TestSubscriptions &s = this->boot();
  EXPECT_TRUE(this->client->subscriptions().empty());
  EXPECT_EQ(this->get()["slots"][1]["status"]["error"],
            "invalid: 'topic' cannot contain '+' or '#': a slot takes one topic");
  EXPECT_EQ(s.state(1), SlotState::OFF);
}

// Every message is published, as upstream's mqtt_subscribe does, and errors are logged when
// they change.
TEST_F(ComponentTest, EveryMessageIsPublishedAndAnErrorLoggedOnce) {
  this->plant({slot_of("Outdoor", "t")});
  TestSubscriptions &s = this->boot();
  int published = 0;
  s.sensor(0)->add_on_state_callback([&published](float) { published++; });
  this->deliver("t", "20");
  this->deliver("t", "20");
  EXPECT_EQ(published, 2);
  LogCapture::instance().clear();
  this->deliver("t", "warm");
  this->deliver("t", "hot");
  EXPECT_EQ(published, 4);
  EXPECT_TRUE(std::isnan(s.sensor(0)->state));
  EXPECT_EQ(s.error(0), "not a number");
  EXPECT_EQ(LogCapture::instance().count("Slot 1 'Outdoor': not a number"), 1u);
  this->deliver("t", "21");
  EXPECT_EQ(s.error(0), "");
  EXPECT_EQ(LogCapture::instance().count("Slot 1 'Outdoor' reads again"), 1u);
}

TEST_F(ComponentTest, AClearedTopicIsUnknown) {
  this->plant(
      {slot_of("N", "n"), slot_of("B", "b", SlotKind::BINARY_SENSOR), slot_of("T", "t", SlotKind::TEXT_SENSOR)});
  TestSubscriptions &s = this->boot();
  this->deliver("n", "5");
  this->deliver("b", "ON");
  this->deliver("t", "x");
  this->deliver("n", "");
  this->deliver("b", "");
  this->deliver("t", "");
  EXPECT_TRUE(std::isnan(s.sensor(0)->state));
  EXPECT_FALSE(s.binary(1)->has_state());
  EXPECT_EQ(s.text(2)->state, "");
  for (size_t i = 0; i < 3; i++)
    EXPECT_EQ(s.error(i), "");
}

TEST_F(ComponentTest, ALargeMessageIsNotReadButShown) {
  this->plant({slot_of("N", "n"), slot_of("T", "t", SlotKind::TEXT_SENSOR)});
  TestSubscriptions &s = this->boot();
  this->deliver("t", "kept");
  const std::string large = "{\"pad\":\"" + std::string(3000, 'x') + "\"}";
  this->deliver("n", large);
  this->deliver("t", large);
  EXPECT_TRUE(std::isnan(s.sensor(0)->state));
  EXPECT_EQ(s.error(0), "message over 2 KiB");
  EXPECT_EQ(s.text(1)->state, "kept");
  EXPECT_EQ(s.raw(0), large.substr(0, RAW_MAX));
}

// A retained value is a level: plain callbacks (automation edges) skip it, full-state ones
// (bindings) get it.
TEST_F(ComponentTest, AnOnOffSlotsFirstValueIsALevel) {
  this->plant({slot_of("Door", "d", SlotKind::BINARY_SENSOR)});
  TestSubscriptions &s = this->boot();
  int plain = 0;
  int full = 0;
  s.binary(0)->add_on_state_callback([&plain](bool) { plain++; });
  s.binary(0)->add_full_state_callback([&full](optional<bool>, optional<bool>) { full++; });
  this->deliver("d", "ON");
  EXPECT_EQ(plain, 0);
  EXPECT_EQ(full, 1);
  this->deliver("d", "OFF");
  EXPECT_EQ(plain, 1);
  EXPECT_EQ(full, 2);
  this->deliver("d", "maybe");
  EXPECT_FALSE(s.binary(0)->has_state());
  EXPECT_EQ(s.error(0), "neither ON nor OFF");
}

TEST_F(ComponentTest, AFullEntityTableLeavesTheSlotOff) {
  static std::vector<sensor::Sensor *> fillers;
  this->plant({slot_of("Outdoor", "t"), slot_of("Door", "d", SlotKind::BINARY_SENSOR)});
  this->boot([](TestSubscriptions &) {
    while (App.get_sensors().size() < App.get_sensors().capacity()) {
      fillers.push_back(new sensor::Sensor());
      App.register_sensor(fillers.back(), "Filler", 0, 0);
    }
  });
  TestSubscriptions &s = *this->subs;
  EXPECT_FALSE(s.active(0));
  EXPECT_TRUE(s.active(1));
  EXPECT_EQ(this->get()["slots"][0]["status"]["error"], "no room in the entity table");
  EXPECT_EQ(this->subscriptions_to("t"), 0u);
  EXPECT_EQ(this->subscriptions_to("d"), 1u);
}

TEST_F(ComponentTest, TheSecondCrashInARowSuspendsTheSlots) {
  this->plant({slot_of("Outdoor", "t")});
  this->board.rtc = armed_streak(1);
  this->board.panic = true;
  TestSubscriptions &s = this->boot();
  EXPECT_TRUE(s.suspended());
  EXPECT_TRUE(this->client->subscriptions().empty());
  // The entity stays, so Home Assistant keeps the same set.
  EXPECT_TRUE(s.active(0));
  EXPECT_EQ(s.state(0), SlotState::SUSPENDED);
  EXPECT_EQ(this->get()["suspended"], true);
  // A restart is what retries them.
  EXPECT_TRUE(s.reboot_required());
  EXPECT_FALSE(this->get()["slots"][0]["pending"].as<bool>());
  EXPECT_EQ(this->get()["slots"][0]["status"]["state"], "suspended");
  // mqtt_config's own setup found the streak already evaluated, and MQTT still runs.
  EXPECT_EQ(this->config->crash_streak(), 2);
  EXPECT_EQ(this->board.rtc.armed, 0);
}

TEST_F(ComponentTest, TheFirstCrashLeavesThemRunning) {
  this->plant({slot_of("Outdoor", "t")});
  this->board.rtc = armed_streak(0);
  this->board.panic = true;
  TestSubscriptions &s = this->boot();
  EXPECT_FALSE(s.suspended());
  EXPECT_EQ(this->subscriptions_to("t"), 1u);
}

// Stored only until the client connects: a first enable later in the boot needs no reboot.
TEST_F(ComponentTest, SlotsWaitForTheBrokerAndThenRead) {
  this->plant({slot_of("Outdoor", "t")});
  TestSubscriptions &s = this->boot();
  EXPECT_EQ(s.state(0), SlotState::WAITING);
  this->client->connect_for_test();
  EXPECT_EQ(s.state(0), SlotState::WAITING);  // no message yet
  this->deliver("t", "4");
  EXPECT_EQ(s.state(0), SlotState::OK);
  this->deliver("t", "x");
  EXPECT_EQ(s.state(0), SlotState::ERROR);
  this->client->drop_for_test(mqtt::MQTTClientDisconnectReason::TCP_DISCONNECTED);
  EXPECT_EQ(s.state(0), SlotState::WAITING);
}

// --- The file ---

TEST_F(ComponentTest, ASaveRewritesTheFileAndKeepsTheOtherSlots) {
  this->plant({outdoor()});
  this->boot();
  // A hand edit since boot, which the save has to keep.
  SlotConfig edited = outdoor();
  edited.decimals = 3;
  this->plant({edited});
  const Answer answer =
      this->post(R"({"slot":3,"enabled":true,"name":"Door","topic":"z2m/door","kind":"binary_sensor"})");
  EXPECT_EQ(answer.result, Result::OK);
  EXPECT_EQ(answer.message, "Slot 3 saved; applies after a reboot");
  EXPECT_TRUE(answer.reboot_required);
  EXPECT_FALSE(exists(this->file() + ".tmp"));
  const std::string text = read_text(this->file());
  const SlotFile file = parse_file(text.data(), text.size(), 4);
  EXPECT_EQ(file.slots[0], edited);
  EXPECT_EQ(file.slots[2].name, "Door");
  EXPECT_EQ(file.slots[2].kind, SlotKind::BINARY_SENSOR);
}

TEST_F(ComponentTest, TheFirstSaveMakesTheFolder) {
  this->boot();
  ASSERT_FALSE(exists(this->folder()));
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"A","topic":"a","kind":"sensor"})").result, Result::OK);
  EXPECT_TRUE(exists(this->file()));
}

TEST_F(ComponentTest, AFailedWriteChangesNothing) {
  this->plant({outdoor()});
  this->boot();
  const std::string before = read_text(this->file());
  mkdir((this->file() + ".tmp").c_str(), 0755);  // the write cannot open it
  const Answer answer = this->post(R"({"slot":2,"enabled":true,"name":"A","topic":"a","kind":"sensor"})");
  EXPECT_EQ(answer.result, Result::STORAGE);
  EXPECT_EQ(answer.message, "Storage unavailable");
  EXPECT_EQ(MqttSubscriptions::http_status(answer.result), 503);
  EXPECT_FALSE(answer.reboot_required);
  EXPECT_EQ(read_text(this->file()), before);
  EXPECT_TRUE(this->get()["slots"][1]["topic"] == "");
  rmdir((this->file() + ".tmp").c_str());
}

TEST_F(ComponentTest, AFolderThatCannotBeMadeIsUnavailableStorage) {
  this->boot();
  write_text(this->folder(), "a file where the folder goes");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"A","topic":"a","kind":"sensor"})").result, Result::STORAGE);
  std::remove(this->folder().c_str());
}

TEST_F(ComponentTest, AWriteThatCannotReplaceTheFileLeavesNoTemporary) {
  this->boot();
  mkdir(this->folder().c_str(), 0755);
  // Folders in the file's place and in the place it would be set aside to: neither moves.
  for (const std::string &path : {this->file(), this->file() + ".bad"}) {
    mkdir(path.c_str(), 0755);
    write_text(path + "/keep", "x");
  }
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"A","topic":"a","kind":"sensor"})").result, Result::STORAGE);
  EXPECT_FALSE(exists(this->file() + ".tmp"));
}

TEST_F(ComponentTest, AStorageGoneFromUnderneathTakesNoSave) {
  this->boot();
  remove_tree(this->storage.get_base_path());
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"A","topic":"a","kind":"sensor"})").result, Result::STORAGE);
}

TEST_F(ComponentTest, AnUnmountedStorageRunsNothingAndTakesNoSave) {
  static dir_storage::DirStorage unmounted;  // never set up
  this->plant({outdoor()});
  TestSubscriptions &s = this->boot([](TestSubscriptions &s) {
    s.set_storage(&unmounted);
    s.set_check_interval(10);
  });
  EXPECT_FALSE(s.active(0));
  // Nothing to look at, so the check looks at nothing.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  App.scheduler.call(millis());
  EXPECT_FALSE(s.reboot_required());
  const Answer answer = this->post(R"({"slot":1,"enabled":true,"name":"A","topic":"a","kind":"sensor"})");
  EXPECT_EQ(answer.result, Result::STORAGE);
  EXPECT_EQ(answer.message, "Storage unavailable");
  EXPECT_EQ(this->get()["file_error"], "unavailable");
}

TEST_F(ComponentTest, AnUnreadableFileIsSetAsideAtBoot) {
  this->plant_text("{\"version\":1,\"slots\":[");
  TestSubscriptions &s = this->boot();
  EXPECT_FALSE(exists(this->file()));
  EXPECT_EQ(read_text(this->file() + ".bad"), "{\"version\":1,\"slots\":[");
  EXPECT_FALSE(s.active(0));
  EXPECT_EQ(this->get()["file_error"], "unreadable: renamed to subscriptions.json.bad");
  EXPECT_FALSE(this->get()["reboot_required"].as<bool>());
  // A save starts the file afresh and the notice goes.
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"A","topic":"a","kind":"sensor"})").result, Result::OK);
  EXPECT_TRUE(this->get()["file_error"].isNull());
}

TEST_F(ComponentTest, AnUnreadableFileThatCannotBeMovedStaysAndRunsNothing) {
  this->plant_text("garbage");
  mkdir((this->file() + ".bad").c_str(), 0755);
  write_text(this->file() + ".bad/keep", "x");
  TestSubscriptions &s = this->boot();
  EXPECT_FALSE(s.active(0));
  EXPECT_EQ(read_text(this->file()), "garbage");
  EXPECT_EQ(LogCapture::instance().count("could not be read, nor renamed"), 1u);
  EXPECT_EQ(this->get()["file_error"], "unreadable");
}

// A read that fails now is not a broken file: nothing is set aside or overwritten.
TEST_F(ComponentTest, AFileThatCannotBeReadAtBootIsLeftAsItIs) {
  this->plant({outdoor()});
  const std::string before = read_text(this->file());
  TestSubscriptions &s = this->boot([](TestSubscriptions &s) { s.fail_reads = true; });
  EXPECT_FALSE(s.active(0));
  EXPECT_EQ(read_text(this->file()), before);
  EXPECT_FALSE(exists(this->file() + ".bad"));
  EXPECT_EQ(LogCapture::instance().count("no slot runs this boot"), 1u);
  EXPECT_EQ(this->get()["file_error"], "unavailable");
  // Once it reads again, the next boot would run the slot.
  s.fail_reads = false;
  JsonDocument doc = this->get();
  EXPECT_TRUE(doc["file_error"].isNull());
  EXPECT_TRUE(doc["reboot_required"].as<bool>());
}

TEST_F(ComponentTest, ASaveWhileTheFileCannotBeReadChangesNothing) {
  SlotConfig door = slot_of("Door", "d", SlotKind::BINARY_SENSOR);
  this->plant({outdoor(), door});
  TestSubscriptions &s = this->boot();
  const std::string before = read_text(this->file());
  s.fail_reads = true;
  const Answer answer = this->post(R"({"slot":3,"enabled":true,"name":"A","topic":"a","kind":"sensor"})");
  EXPECT_EQ(answer.result, Result::STORAGE);
  EXPECT_EQ(answer.message, "Storage unavailable");
  EXPECT_FALSE(answer.reboot_required);
  EXPECT_EQ(read_text(this->file()), before);
  EXPECT_FALSE(exists(this->file() + ".bad"));
  JsonDocument doc = this->get();
  EXPECT_EQ(doc["file_error"], "unavailable");
  EXPECT_EQ(doc["slots"][1]["name"], "Door");  // what was read before stands
  s.fail_reads = false;
  EXPECT_EQ(this->post(R"({"slot":3,"enabled":true,"name":"A","topic":"a","kind":"sensor"})").result, Result::OK);
  const std::string text = read_text(this->file());
  EXPECT_EQ(parse_file(text.data(), text.size(), 4).slots[1], door);
}

TEST_F(ComponentTest, AStatThatFailsIsNoVerdictEither) {
  this->plant({outdoor()});
  TestSubscriptions &s = this->boot([](TestSubscriptions &s) {
    s.fail_stat = true;
    s.set_check_interval(10);
  });
  EXPECT_FALSE(s.active(0));
  EXPECT_EQ(this->post(R"({"slot":2,"action":"clear"})").result, Result::STORAGE);
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  App.scheduler.call(millis());
  EXPECT_FALSE(s.reboot_required());
  EXPECT_TRUE(exists(this->file()));
}

// What was last seen of the file stays as it was, so the next look tries again.
TEST_F(ComponentTest, TheCheckTriesAgainAfterAReadThatFailed) {
  this->plant({outdoor()});
  TestSubscriptions &s = this->boot([](TestSubscriptions &s) { s.set_check_interval(10); });
  SlotConfig moved = outdoor();
  moved.topic = "zigbee2mqtt/outdoor_sensor";
  this->plant({moved});
  s.fail_reads = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  App.scheduler.call(millis());
  EXPECT_FALSE(s.reboot_required());
  s.fail_reads = false;
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  App.scheduler.call(millis());
  EXPECT_TRUE(s.reboot_required());
}

// Rewritten into garbage while running: reported, and set aside only by the save that replaces it.
TEST_F(ComponentTest, AFileBrokenWhileRunningIsReportedAndSetAsideByTheNextSave) {
  this->plant({outdoor()});
  this->boot();
  this->plant_text("garbage");
  JsonDocument doc = this->get();
  EXPECT_EQ(doc["file_error"], "unreadable");
  EXPECT_TRUE(doc["reboot_required"].as<bool>());  // the next boot would run nothing
  EXPECT_TRUE(exists(this->file()));
  EXPECT_EQ(this->post(R"({"slot":2,"enabled":true,"name":"A","topic":"a","kind":"sensor"})").result, Result::OK);
  EXPECT_EQ(read_text(this->file() + ".bad"), "garbage");
}

TEST_F(ComponentTest, NewerFirmwaresFileIsLeftAsItIs) {
  const std::string newer =
      R"({"version":2,"slots":[{"slot":1,"enabled":true,"name":"A","topic":"a","kind":"sensor","qos":1}]})";
  this->plant_text(newer);
  TestSubscriptions &s = this->boot();
  EXPECT_FALSE(s.active(0));
  EXPECT_TRUE(this->client->subscriptions().empty());
  JsonDocument doc = this->get();
  EXPECT_EQ(doc["file_error"], "newer_firmware");
  EXPECT_FALSE(doc["reboot_required"].as<bool>());
  for (const char *body :
       {R"({"slot":1,"enabled":true,"name":"B","topic":"b","kind":"sensor"})", R"({"slot":1,"action":"clear"})"}) {
    const Answer answer = this->post(body);
    EXPECT_EQ(answer.result, Result::NEWER_FILE);
    EXPECT_EQ(answer.message, "The subscriptions file was written by newer firmware; it is left as it is");
    EXPECT_EQ(MqttSubscriptions::http_status(answer.result), 503);
  }
  EXPECT_EQ(read_text(this->file()), newer);
  LogCapture::instance().clear();
  s.dump_config();
  EXPECT_EQ(LogCapture::instance().count("The file is newer firmware's; nothing runs"), 1u);
}

// --- Saves, clears and what waits for a reboot ---

TEST_F(ComponentTest, WhatASaveSaysAndWhatWaits) {
  this->plant({outdoor()});
  TestSubscriptions &s = this->boot();
  const char *same =
      R"({"slot":1,"enabled":true,"name":"Outdoor temperature","topic":"zigbee2mqtt/outdoor","kind":"sensor",)"
      R"("json_path":"temperature","unit":"°C","decimals":2})";
  const char *changed =
      R"({"slot":1,"enabled":true,"name":"Outdoor temperature","topic":"zigbee2mqtt/outdoor","kind":"sensor",)"
      R"("json_path":"temperature","unit":"°C","decimals":1})";
  EXPECT_EQ(this->post(same).message, "Nothing changed");
  Answer answer = this->post(changed);
  EXPECT_EQ(answer.message, "Slot 1 saved; applies after a reboot");
  EXPECT_TRUE(answer.reboot_required);
  EXPECT_TRUE(s.reboot_required());
  EXPECT_TRUE(this->get()["slots"][0]["pending"].as<bool>());
  EXPECT_FALSE(this->get()["slots"][1]["pending"].as<bool>());
  // Back to what runs.
  answer = this->post(same);
  EXPECT_EQ(answer.message, "Slot 1 saved");
  EXPECT_FALSE(answer.reboot_required);
  EXPECT_FALSE(this->get()["slots"][0]["pending"].as<bool>());
}

TEST_F(ComponentTest, WhatAClearSays) {
  SlotConfig disabled = slot_of("Spare", "spare");
  disabled.enabled = false;
  this->plant({outdoor(), disabled});
  this->boot();
  Answer answer = this->post(R"({"slot":1,"action":"clear"})");
  EXPECT_EQ(answer.message, "Slot 1 cleared; its entity goes after a reboot");
  EXPECT_TRUE(answer.reboot_required);
  JsonDocument doc = this->get();
  EXPECT_EQ(doc["slots"][0]["topic"], "");
  EXPECT_FALSE(doc["slots"][0]["entity"].isNull());  // still runs this boot
  // A slot that runs nothing goes without a reboot.
  answer = this->post(R"({"slot":2,"action":"clear"})");
  EXPECT_EQ(answer.message, "Slot 2 cleared");
  EXPECT_EQ(this->post(R"({"slot":2,"action":"clear"})").message, "Nothing changed");
  EXPECT_EQ(this->post(R"({"slot":4,"action":"clear"})").message, "Nothing changed");
  // A clear ignores the rest of the body.
  EXPECT_EQ(this->post(R"({"slot":3,"action":"clear","name":7})").message, "Nothing changed");
}

TEST_F(ComponentTest, EditingASlotThatDoesNotRunNeedsNoReboot) {
  SlotConfig disabled = slot_of("Spare", "spare");
  disabled.enabled = false;
  this->plant({disabled});
  this->boot();
  const Answer answer = this->post(R"({"slot":1,"enabled":false,"name":"Spare 2","topic":"other","kind":"sensor"})");
  EXPECT_EQ(answer.message, "Slot 1 saved");
  EXPECT_FALSE(answer.reboot_required);
}

// An enabled slot that did not start runs nothing either way, so clearing or disabling it waits
// for no reboot; fixing it does.
TEST_F(ComponentTest, ASlotThatDidNotStartIsPendingOnlyOnceTheNextBootWouldRunIt) {
  SlotConfig taken = slot_of("Input 1", "a", SlotKind::BINARY_SENSOR);  // the name of a YAML input
  SlotConfig wildcard = slot_of("Wild", "a/#");
  this->plant({taken, wildcard});
  TestSubscriptions &s = this->boot();
  ASSERT_FALSE(s.active(0));
  ASSERT_FALSE(s.active(1));
  EXPECT_FALSE(s.reboot_required());
  EXPECT_FALSE(this->get()["slots"][0]["pending"].as<bool>());

  // Turned off, under a name a save takes.
  Answer answer = this->post(R"({"slot":1,"enabled":false,"name":"Spare","topic":"a","kind":"binary_sensor"})");
  EXPECT_EQ(answer.message, "Slot 1 saved");
  EXPECT_FALSE(answer.reboot_required);
  answer = this->post(R"({"slot":2,"action":"clear"})");
  EXPECT_EQ(answer.message, "Slot 2 cleared");
  EXPECT_FALSE(answer.reboot_required);

  answer = this->post(R"({"slot":1,"enabled":true,"name":"Door","topic":"a","kind":"binary_sensor"})");
  EXPECT_EQ(answer.message, "Slot 1 saved; applies after a reboot");
  EXPECT_TRUE(answer.reboot_required);
  EXPECT_TRUE(this->get()["slots"][0]["pending"].as<bool>());
}

// A running slot turned off waits for the reboot that removes its entity.
TEST_F(ComponentTest, DisablingARunningSlotWaitsForTheReboot) {
  this->plant({outdoor()});
  this->boot();
  const Answer answer = this->post(
      R"({"slot":1,"enabled":false,"name":"Outdoor temperature","topic":"zigbee2mqtt/outdoor","kind":"sensor",)"
      R"("json_path":"temperature","unit":"°C","decimals":2})");
  EXPECT_EQ(answer.message, "Slot 1 saved; applies after a reboot");
  EXPECT_TRUE(answer.reboot_required);
}

TEST_F(ComponentTest, RefusedBodies) {
  this->boot();
  const std::string range = "'slot' must be a whole number from 1 to 4";
  for (const char *body : {R"({"action":"clear"})", R"({"slot":0,"action":"clear"})", R"({"slot":5,"action":"clear"})",
                           R"({"slot":300,"action":"clear"})", R"({"slot":"1","action":"clear"})",
                           R"({"slot":1.5,"action":"clear"})", R"({"slot":true})"}) {
    SCOPED_TRACE(body);
    const Answer answer = this->post(body);
    EXPECT_EQ(answer.result, Result::INVALID);
    EXPECT_EQ(answer.message, range);
    EXPECT_EQ(MqttSubscriptions::http_status(answer.result), 400);
  }
  EXPECT_EQ(this->post(R"({"slot":1,"action":"remove"})").message, "'action' must be 'clear'");
  EXPECT_EQ(this->post(R"({"slot":1,"action":null})").message, "'action' must be 'clear'");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"A","topic":"a","kind":"sensor","unit":"K"})").message,
            "'unit' is not one this firmware offers");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"topic":"a","kind":"sensor"})").message, "'name' is required");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"A","kind":"sensor"})").message, "'topic' is required");
  EXPECT_EQ(this->post(R"({"slot":1,"enabled":true,"name":"A","topic":"a","kind":"sensor","x":1})").message,
            "'x' is not a slot field");
  EXPECT_FALSE(exists(this->file()));
  EXPECT_EQ(MqttSubscriptions::http_status(Result::OK), 200);
}

// A Backup restore or a /files upload says nothing; the check notices the file changed.
TEST_F(ComponentTest, AFileRewrittenBehindItsBackIsNoticed) {
  this->plant({outdoor()});
  TestSubscriptions &s = this->boot([](TestSubscriptions &s) { s.set_check_interval(10); });
  EXPECT_FALSE(s.reboot_required());
  SlotConfig moved = outdoor();
  moved.topic = "zigbee2mqtt/outdoor_sensor";
  this->plant({moved});
  EXPECT_FALSE(s.reboot_required());
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  App.scheduler.call(millis());
  EXPECT_TRUE(s.reboot_required());
  // Unchanged since: nothing to read again.
  this->plant({outdoor()});
  JsonDocument doc = this->get();
  EXPECT_FALSE(doc["reboot_required"].as<bool>());
}

TEST_F(ComponentTest, TheGetShape) {
  this->plant({outdoor(), slot_of("Door", "z2m/door", SlotKind::BINARY_SENSOR)});
  TestSubscriptions &s = this->boot();
  this->client->connect_for_test();
  s.now = 5000;
  this->deliver("zigbee2mqtt/outdoor", R"({"battery":97,"temperature":21.4,"humidity":48})");
  s.now = 17500;
  JsonDocument doc = this->get();
  EXPECT_EQ(doc["max_slots"], 4);
  EXPECT_EQ(doc["reboot_required"], false);
  EXPECT_EQ(doc["suspended"], false);
  EXPECT_TRUE(doc["file_error"].isNull());
  ASSERT_EQ(doc["units"].size(), 2u);
  EXPECT_EQ(doc["units"][0], "°C");
  ASSERT_EQ(doc["slots"].size(), 4u);

  JsonObject first = doc["slots"][0];
  EXPECT_EQ(first["slot"], 1);
  EXPECT_EQ(first["enabled"], true);
  EXPECT_EQ(first["name"], "Outdoor temperature");
  EXPECT_EQ(first["topic"], "zigbee2mqtt/outdoor");
  EXPECT_EQ(first["kind"], "sensor");
  EXPECT_EQ(first["json_path"], "temperature");
  EXPECT_EQ(first["unit"], "°C");
  EXPECT_EQ(first["decimals"], 2);
  EXPECT_EQ(first["payload_on"], "ON");
  EXPECT_EQ(first["payload_off"], "OFF");
  EXPECT_EQ(first["pending"], false);
  EXPECT_EQ(first["entity"]["domain"], "sensor");
  EXPECT_EQ(first["entity"]["name"], "Outdoor temperature");
  EXPECT_EQ(first["status"]["state"], "ok");
  EXPECT_EQ(first["status"]["value"], "21.40 °C");
  EXPECT_EQ(first["status"]["raw"], R"({"battery":97,"temperature":21.4,"humidity":48})");
  EXPECT_TRUE(first["status"]["error"].isNull());
  EXPECT_EQ(first["status"]["age_s"], 12);

  JsonObject door = doc["slots"][1];
  EXPECT_EQ(door["entity"]["domain"], "binary_sensor");
  EXPECT_EQ(door["status"]["state"], "waiting");
  EXPECT_EQ(door["status"]["value"], "");
  EXPECT_TRUE(door["status"]["age_s"].isNull());

  JsonObject empty = doc["slots"][3];
  EXPECT_EQ(empty["slot"], 4);
  EXPECT_EQ(empty["enabled"], false);
  EXPECT_EQ(empty["topic"], "");
  EXPECT_EQ(empty["decimals"], 1);
  EXPECT_TRUE(empty["entity"].isNull());
  EXPECT_EQ(empty["status"]["state"], "off");
  EXPECT_TRUE(empty["status"]["error"].isNull());
}

TEST_F(ComponentTest, DumpConfigNamesWhatRunsAndWhy) {
  this->plant({outdoor(), slot_of("Bad", "a+b")});
  TestSubscriptions &s = this->boot();
  LogCapture::instance().clear();
  s.dump_config();
  EXPECT_EQ(LogCapture::instance().count("Slots running: 1 of 4"), 1u);
  EXPECT_EQ(LogCapture::instance().count("'Outdoor temperature' (sensor) on 'zigbee2mqtt/outdoor'"), 1u);
  EXPECT_EQ(LogCapture::instance().count("Slot 2: off, invalid:"), 1u);
}

}  // namespace esphome::mqtt_subscriptions::testing
