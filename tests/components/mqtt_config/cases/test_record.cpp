#include "common.h"

namespace esphome::mqtt_config::testing {

// The bytes are the format: older and newer firmware read the same 512.
TEST(MqttRecordLayout, IsFiveHundredTwelveBytesWithoutPadding) {
  EXPECT_EQ(sizeof(StoredMqttV1), 512u);
  EXPECT_EQ(offsetof(StoredMqttV1, flags), 2u);
  EXPECT_EQ(offsetof(StoredMqttV1, port), 4u);
  EXPECT_EQ(offsetof(StoredMqttV1, broker), 8u);
  EXPECT_EQ(offsetof(StoredMqttV1, reserved), 461u);
}

TEST(MqttRecordLayout, RoundTripsEveryField) {
  MqttRecord record;
  record.enabled = true;
  record.discovery = true;
  record.clean_pending = true;
  record.port = 8883;
  record.broker = "broker.local";
  record.username = "jxd";
  record.password = "pw";
  record.client_id = "jxd-1";
  record.topic_prefix = "home/jxd";
  const StoredMqttV1 stored = to_stored(record, StoredMqttV1{});
  EXPECT_EQ(stored.layout, 1);
  EXPECT_EQ(stored.min_reader, 1);
  EXPECT_EQ(stored.flags, FLAG_ENABLED | FLAG_DISCOVERY | FLAG_CLEAN_PENDING);
  const MqttRecord back = from_stored(stored);
  EXPECT_TRUE(back.same_settings(record));
  EXPECT_TRUE(back.clean_pending);
}

TEST(MqttRecordLayout, FieldsAtTheirLimitsRoundTrip) {
  MqttRecord record;
  record.broker = std::string(BROKER_MAX, 'b');
  record.username = std::string(USERNAME_MAX, 'u');
  record.password = std::string(PASSWORD_MAX, 'p');
  record.client_id = std::string(CLIENT_ID_MAX, 'c');
  record.topic_prefix = std::string(TOPIC_PREFIX_MAX, 't');
  EXPECT_TRUE(from_stored(to_stored(record, StoredMqttV1{})).same_settings(record));
}

// A record whose fields have lost their terminators still reads as text that fits.
TEST(MqttRecordLayout, ACorruptedRecordReadsAsFieldsCutAtTheirBuffers) {
  StoredMqttV1 garbage;
  std::memset(&garbage, 'A', sizeof(garbage));
  const MqttRecord record = from_stored(garbage);
  EXPECT_EQ(record.broker, std::string(BROKER_MAX, 'A'));
  EXPECT_EQ(record.username, std::string(USERNAME_MAX, 'A'));
  EXPECT_EQ(record.password, std::string(PASSWORD_MAX, 'A'));
  EXPECT_EQ(record.client_id, std::string(CLIENT_ID_MAX, 'A'));
  EXPECT_EQ(record.topic_prefix, std::string(TOPIC_PREFIX_MAX, 'A'));
}

// Only a record validate() refused can carry one, and it must not spill into the next field.
TEST(MqttRecordLayout, AFieldTooLongForItsBufferIsCut) {
  MqttRecord record;
  record.username = std::string(USERNAME_MAX + 10, 'u');
  record.password = "pw";
  const StoredMqttV1 stored = to_stored(record, StoredMqttV1{});
  const MqttRecord back = from_stored(stored);
  EXPECT_EQ(back.username, std::string(USERNAME_MAX, 'u'));
  EXPECT_EQ(back.password, "pw");
}

TEST(MqttRecordLayout, AWriteKeepsTheFlagBitsAndBytesItDoesNotKnow) {
  StoredMqttV1 base = to_stored(MqttRecord{}, StoredMqttV1{});
  base.flags |= 0x80;
  base.reserved8 = 0x11;
  base.reserved16 = 0x2233;
  base.reserved[0] = 0x44;
  base.reserved[50] = 0x55;
  MqttRecord record;
  record.enabled = true;
  const StoredMqttV1 out = to_stored(record, base);
  EXPECT_EQ(out.flags, 0x80 | FLAG_ENABLED);
  EXPECT_EQ(out.reserved8, 0x11);
  EXPECT_EQ(out.reserved16, 0x2233);
  EXPECT_EQ(out.reserved[0], 0x44);
  EXPECT_EQ(out.reserved[50], 0x55);
}

TEST(MqttRecordLayout, SameSettingsIgnoresTheDevicesOwnBookkeeping) {
  MqttRecord a = enabled_record();
  MqttRecord b = a;
  b.clean_pending = true;
  EXPECT_TRUE(a.same_settings(b));
  b.port = 1884;
  EXPECT_FALSE(a.same_settings(b));
  b = a;
  b.password = "pw";
  EXPECT_FALSE(a.same_settings(b));
}

TEST(MqttRecordLayout, RunningSettingsCompareWithTheDefaultsFilledIn) {
  MqttRecord stored = enabled_record();
  MqttRecord running = stored;
  running.client_id = "default-id";
  running.topic_prefix = "default-prefix";
  EXPECT_TRUE(same_running_settings(stored, running, "default-id", "default-prefix"));
  stored.client_id = "other";
  EXPECT_FALSE(same_running_settings(stored, running, "default-id", "default-prefix"));
  stored.client_id = "";
  stored.discovery = true;
  EXPECT_FALSE(same_running_settings(stored, running, "default-id", "default-prefix"));
  stored.discovery = false;
  stored.password = "pw";
  EXPECT_FALSE(same_running_settings(stored, running, "default-id", "default-prefix"));
}

class RecordStorageTest : public MqttTest {};

TEST_F(RecordStorageTest, NothingStoredGivesTheDefaults) {
  TestConfig &c = this->boot();
  JsonDocument doc = this->settings_json();
  EXPECT_FALSE(doc["enabled"].as<bool>());
  EXPECT_EQ(doc["port"].as<int>(), 1883);
  EXPECT_TRUE(doc["stored_notice"].isNull());
  EXPECT_EQ(c.state(), MqttState::NOT_CONFIGURED);
  EXPECT_EQ(c.stores, 0);
}

// NVS keeps a blob of another size under the key; the preference does not load it.
TEST_F(RecordStorageTest, ARecordOfAnotherSizeReadsAsNothingStored) {
  this->board.nvs.assign(256, 0xFF);
  TestConfig &c = this->boot();
  EXPECT_EQ(c.state(), MqttState::NOT_CONFIGURED);
  EXPECT_TRUE(this->settings_json()["stored_notice"].isNull());
  EXPECT_EQ(this->board.nvs.size(), 256u);
}

TEST_F(RecordStorageTest, ASaveKeepsWhatNewerFirmwareAddedToALayoutOneRecord) {
  StoredMqttV1 planted = record_of(MqttRecord{});
  planted.layout = 2;  // written by a newer layout that older readers may still apply
  planted.flags |= 0x40;
  planted.reserved[3] = 0xAB;
  this->board.write(planted);
  this->boot();
  ASSERT_EQ(this->save(patch_of([](MqttPatch &p) { p.port = 1884; })).code, 200);
  const StoredMqttV1 out = this->board.record();
  EXPECT_EQ(out.port, 1884);
  EXPECT_EQ(out.flags & 0x40, 0x40);
  EXPECT_EQ(out.reserved[3], 0xAB);
  EXPECT_EQ(out.layout, 1);
  EXPECT_EQ(out.min_reader, 1);
}

// After a rollback: the record is not this firmware's to apply, and nothing rewrites it
// until someone saves on purpose.
TEST_F(RecordStorageTest, NewerFirmwaresRecordIsNeitherAppliedNorWrittenImplicitly) {
  StoredMqttV1 planted = record_of(enabled_record());
  planted.layout = 2;
  planted.min_reader = 2;
  planted.reserved[7] = 0x99;
  this->board.write(planted);
  TestConfig &c = this->boot();
  EXPECT_FALSE(c.running());
  EXPECT_EQ(this->client->enable_calls, 0);
  EXPECT_EQ(c.state(), MqttState::NOT_CONFIGURED);
  EXPECT_FALSE(c.reboot_required());
  EXPECT_EQ(this->settings_json()["stored_notice"].as<std::string>(), "newer_firmware");
  EXPECT_EQ(c.stores, 0);
  EXPECT_EQ(this->board.record().min_reader, 2);

  // Saving nothing new leaves it be.
  EXPECT_STREQ(this->save(MqttPatch{}).message, "Nothing changed");
  EXPECT_EQ(c.stores, 0);
}

TEST_F(RecordStorageTest, AnExplicitSaveReplacesNewerFirmwaresRecordWithAFreshOne) {
  StoredMqttV1 planted = record_of(enabled_record());
  planted.min_reader = 2;
  planted.flags |= 0x80;
  planted.reserved[7] = 0x99;
  this->board.write(planted);
  this->boot();
  const auto result = this->save(patch_of([](MqttPatch &p) { p.broker = std::string("10.0.2.2"); }));
  EXPECT_EQ(result.code, 200);
  const StoredMqttV1 out = this->board.record();
  EXPECT_EQ(out.min_reader, 1);
  EXPECT_EQ(out.flags, 0);  // the defaults with the broker, nothing of the foreign record
  EXPECT_EQ(out.reserved[7], 0);
  EXPECT_EQ(from_stored(out).broker, "10.0.2.2");
  EXPECT_TRUE(this->settings_json()["stored_notice"].isNull());
}

}  // namespace esphome::mqtt_config::testing
