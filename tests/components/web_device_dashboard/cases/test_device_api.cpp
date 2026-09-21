#include "common.h"
#include "esphome/core/version.h"

namespace esphome::web_device_dashboard::testing {

TEST_F(Dashboard, InfoNamesTheDeviceAndTheFramework) {
  Reply reply = this->get("/api/device/info");
  ASSERT_EQ(reply.code, 200);
  // The friendly name when there is one; App.get_name() otherwise.
  EXPECT_EQ(reply["name"].as<std::string>(), "Dashboard Test");
  EXPECT_EQ(reply["version"].as<std::string>(), ESPHOME_VERSION);
}

TEST_F(Dashboard, InfoReportsTheChipMacTwiceWithoutAnEthernetLink) {
  Reply reply = this->get("/api/device/info");
  const std::string base = reply["base_mac_address"].as<std::string>();
  EXPECT_EQ(base.size(), 17u) << base;
  EXPECT_EQ(base[2], ':');
  // The active MAC is the ethernet one where there is ethernet; here it is the base MAC.
  EXPECT_EQ(reply["mac_address"].as<std::string>(), base);
}

TEST_F(Dashboard, InfoReportsABoardThatWasNeverRead) {
  Reply reply = this->get("/api/device/info");
  ASSERT_FALSE(reply["board"].isNull());
  EXPECT_FALSE(reply["board"]["valid"].as<bool>());
  // Nothing beyond the flag: an unread EEPROM has no fields to report.
  EXPECT_TRUE(reply["board"]["boardname"].isNull());
  EXPECT_TRUE(reply["serial_number"].isNull());
  EXPECT_TRUE(reply["device_model"].isNull());
}

TEST_F(Dashboard, InfoCarriesTheBoardHeaderVerbatim) {
  this->board.read_header();
  Reply reply = this->get("/api/device/info");
  JsonVariant board = reply["board"];
  EXPECT_TRUE(board["valid"].as<bool>());
  EXPECT_EQ(board["header_version"].as<int>(), 4);
  EXPECT_EQ(board["boardname"].as<std::string>(), "JetHub D1");
  EXPECT_EQ(board["boardversion"].as<std::string>(), "4");
  EXPECT_EQ(board["board_serial"].as<std::string>(), "B0001");
  EXPECT_EQ(board["cpuid"].as<std::string>(), "0123456789abcdef");
  EXPECT_EQ(board["mac"].as<std::string>(), "001E06AABBCC");
  EXPECT_EQ(board["timestamp"].as<int64_t>(), 1700000000);
  EXPECT_EQ(board["signature_version"].as<int>(), jethome_board_info::SIG_NONE);
  // No device.id record: the identity is reported as absent, not left out.
  EXPECT_TRUE(board["identity"].isNull());
}

TEST_F(Dashboard, AnUnsignedBoardCarriesNoSignature) {
  this->board.read_header();
  Reply reply = this->get("/api/device/info");
  EXPECT_FALSE(reply["board"]["signature"].is<const char *>());
}

TEST_F(Dashboard, ASignedBoardCarriesItsSignatureAsHex) {
  this->board.read_header();
  this->board.sign(2);
  Reply reply = this->get("/api/device/info");
  EXPECT_EQ(reply["board"]["signature_version"].as<int>(), 2);
  const std::string signature = reply["board"]["signature"].as<std::string>();
  EXPECT_EQ(signature.size(), 2 * jethome_board_info::SIGNATURE_SIZE);
  EXPECT_EQ(signature.substr(0, 4), "abab");
}

TEST_F(Dashboard, TheIdentityCardFieldsAreHoistedToTheRoot) {
  this->board.read_header();
  this->board.read_device_identity();
  Reply reply = this->get("/api/device/info");
  JsonVariant identity = reply["board"]["identity"];
  ASSERT_FALSE(identity.isNull());
  EXPECT_EQ(identity["model"].as<std::string>(), "JetHub D1+");
  EXPECT_EQ(identity["serial"].as<std::string>(), "0000000042");
  EXPECT_EQ(identity["hw_revision"].as<std::string>(), "1.2");
  // What the page's identity card reads, alongside the board object.
  EXPECT_EQ(reply["serial_number"].as<std::string>(), "0000000042");
  EXPECT_EQ(reply["device_model"].as<std::string>(), "JetHub D1+");
  EXPECT_EQ(reply["hw_revision"].as<std::string>(), "1.2");
}

TEST_F(Dashboard, TheSerialCrossCheckIsReportedOnlyWhenItRan) {
  this->board.read_header();
  this->board.read_device_identity();
  Reply unchecked = this->get("/api/device/info");
  EXPECT_TRUE(unchecked["board"]["identity"]["serial_matches_usid"].isNull());

  this->board.check_serial(false);
  Reply checked = this->get("/api/device/info");
  EXPECT_FALSE(checked["board"]["identity"]["serial_matches_usid"].as<bool>());
}

TEST_F(Dashboard, StatusReportsAHostWithNoLink) {
  Reply reply = this->get("/api/device/status");
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply["connection_type"].as<std::string>(), "none");
  EXPECT_TRUE(reply["rssi"].isNull());
  EXPECT_TRUE(reply["ip_address"].isNull());
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
  // esp_reset_reason() is ESP32 only; everywhere else the page is told so.
  EXPECT_EQ(reply["reset_reason"].as<std::string>(), "unknown");
}

TEST_F(Dashboard, StatusCountsUptimeInSeconds) {
  Reply reply = this->get("/api/device/status");
  EXPECT_TRUE(reply["uptime_s"].is<uint32_t>());
  EXPECT_EQ(reply["uptime_s"].as<uint32_t>(), millis() / 1000);
}

TEST_F(Dashboard, NetworkReportsTheHostnameAndNoLink) {
  Reply reply = this->get("/api/device/network");
  ASSERT_EQ(reply.code, 200);
  // The device name, not the friendly one: this is the hostname on the wire.
  EXPECT_EQ(reply["hostname"].as<std::string>(), "dashboard-test");
  EXPECT_EQ(reply["connection_type"].as<std::string>(), "none");
  EXPECT_FALSE(reply["ethernet_connected"].as<bool>());
}

TEST_F(Dashboard, NetworkSpellsOutEveryFieldItCouldNotFill) {
  Reply reply = this->get("/api/device/network");
  for (const char *key : {"ip_address", "gateway", "subnet", "dns1", "dns2", "ssid", "rssi"})
    EXPECT_TRUE(reply[key].isNull()) << key;
}

}  // namespace esphome::web_device_dashboard::testing
