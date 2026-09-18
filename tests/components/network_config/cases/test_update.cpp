#include "common.h"

namespace esphome::network_config::testing {

// The record every update case starts from.
static const char *const STORED =
    R"({"network_type":"ethernet",)"
    R"("wifi":{"use_dhcp":false,"manual_ip":{"static_ip":"192.168.1.50","gateway":"192.168.1.1",)"
    R"("subnet":"255.255.255.0","dns1":"192.168.1.1","dns2":""}},)"
    R"("ethernet":{"use_dhcp":true,"manual_ip":{"static_ip":"","gateway":"","subnet":"","dns1":"","dns2":""}},)"
    R"("wifi_networks":[{"ssid":"home","password":"s3cret00"},{"ssid":"shop","password":""}]})";

class Update : public Record {
 protected:
  void SetUp() override {
    Record::SetUp();
    ASSERT_TRUE(this->load(STORED));
    this->settings.clear_dirty();
  }
};

TEST_F(Update, AnEmptyBodyChangesNothingAndIsAccepted) {
  const std::string before = this->report();
  EXPECT_EQ(this->update("{}"), nullptr);
  EXPECT_EQ(this->report(), before);
}

TEST_F(Update, TakesTheModeAndMarksTheRecordDirty) {
  ASSERT_EQ(this->update(R"({"network_type":"auto"})"), nullptr);
  EXPECT_EQ(this->settings.network_type(), NetworkType::AUTO);
  EXPECT_TRUE(this->settings.is_dirty());
}

TEST_F(Update, TheNetworkListIsReplacedWhole) {
  ASSERT_EQ(this->update(R"({"wifi_networks":[{"ssid":"office","password":"officepass"}]})"), nullptr);
  ASSERT_EQ(this->settings.networks().size(), 1U);
  EXPECT_EQ(this->settings.networks()[0].ssid, "office");
}

TEST_F(Update, AnEmptyListClearsTheNetworks) {
  ASSERT_EQ(this->update(R"({"wifi_networks":[]})"), nullptr);
  EXPECT_TRUE(this->settings.networks().empty());
}

TEST_F(Update, AnEntryWithNoPasswordKeepsTheStoredOne) {
  // What a list read back from GET, which carries no passwords, round-trips with.
  ASSERT_EQ(this->update(R"({"wifi_networks":[{"ssid":"shop"},{"ssid":"home"}]})"), nullptr);
  ASSERT_EQ(this->settings.networks().size(), 2U);
  EXPECT_EQ(this->settings.networks()[0].ssid, "shop");
  EXPECT_EQ(this->settings.networks()[0].password, "");
  EXPECT_EQ(this->settings.networks()[1].password, "s3cret00");
}

TEST_F(Update, AnEmptyPasswordOpensTheNetwork) {
  ASSERT_EQ(this->update(R"({"wifi_networks":[{"ssid":"home","password":""}]})"), nullptr);
  EXPECT_EQ(this->settings.networks()[0].password, "");
}

TEST_F(Update, AnUnknownSsidWithNoPasswordIsStoredOpen) {
  ASSERT_EQ(this->update(R"({"wifi_networks":[{"ssid":"guest"}]})"), nullptr);
  EXPECT_EQ(this->settings.networks()[0].password, "");
}

TEST_F(Update, FiveEntriesFitAndSixDoNot) {
  ASSERT_EQ(this->update(R"({"wifi_networks":[{"ssid":"1"},{"ssid":"2"},{"ssid":"3"},{"ssid":"4"},{"ssid":"5"}]})"),
            nullptr);
  EXPECT_EQ(this->settings.networks().size(), MAX_NETWORKS);
}

TEST_F(Update, ThirtyTwoByteSsidsAndSixtyFourByteKeysAreTaken) {
  const std::string ssid(SSID_MAX_LEN, 's');
  const std::string password(PASSWORD_MAX_LEN, 'p');
  ASSERT_EQ(this->update(R"({"wifi_networks":[{"ssid":")" + ssid + R"(","password":")" + password + R"("}]})"),
            nullptr);
  EXPECT_EQ(this->settings.networks()[0].ssid, ssid);
  EXPECT_EQ(this->settings.networks()[0].password, password);
}

TEST_F(Update, AnSsidIsCountedInBytes) {
  // 802.11 counts octets, so sixteen two-byte glyphs fill an SSID and a seventeenth is one too
  // many, however few characters that reads as.
  const std::string glyph = "ф";
  ASSERT_EQ(glyph.size(), 2U);
  std::string fits;
  for (size_t i = 0; i < SSID_MAX_LEN / 2; i++)
    fits += glyph;
  EXPECT_EQ(this->update(R"({"wifi_networks":[{"ssid":")" + fits + R"("}]})"), nullptr);
  EXPECT_STREQ(this->update(R"({"wifi_networks":[{"ssid":")" + fits + glyph + R"("}]})"),
               "'ssid' must be 1 to 32 characters");
}

TEST_F(Update, SsidsDifferingOnlyInCaseAreTwoNetworks) {
  ASSERT_EQ(this->update(R"({"wifi_networks":[{"ssid":"Home"},{"ssid":"home"}]})"), nullptr);
  EXPECT_EQ(this->settings.networks().size(), 2U);
}

TEST_F(Update, TheInterfaceBlocksMergeFieldByField) {
  // Flipping DHCP on must not discard the addresses the user can flip back to.
  ASSERT_EQ(this->update(R"({"wifi":{"use_dhcp":true}})"), nullptr);
  EXPECT_TRUE(this->settings.wifi().use_dhcp);
  EXPECT_EQ(this->settings.wifi().manual_ip.static_ip, "192.168.1.50");

  ASSERT_EQ(this->update(R"({"wifi":{"manual_ip":{"dns2":"9.9.9.9"}}})"), nullptr);
  EXPECT_EQ(this->settings.wifi().manual_ip.dns2, "9.9.9.9");
  EXPECT_EQ(this->settings.wifi().manual_ip.gateway, "192.168.1.1");
}

TEST_F(Update, AnAddressMayBeClearedWithAnEmptyString) {
  ASSERT_EQ(this->update(R"({"ethernet":{"manual_ip":{"dns1":"1.1.1.1"}}})"), nullptr);
  ASSERT_EQ(this->update(R"({"ethernet":{"manual_ip":{"dns1":""}}})"), nullptr);
  EXPECT_EQ(this->settings.ethernet().manual_ip.dns1, "");
}

TEST_F(Update, AStaticInterfaceNeedsAddressGatewayAndSubnetTogether) {
  ASSERT_EQ(this->update(R"({"ethernet":{"use_dhcp":false,"manual_ip":{"static_ip":"10.0.0.7",)"
                         R"("gateway":"10.0.0.1","subnet":"255.255.255.0"}}})"),
            nullptr);
  EXPECT_FALSE(this->settings.ethernet().use_dhcp);
  EXPECT_EQ(this->settings.ethernet().manual_ip.gateway, "10.0.0.1");
}

TEST_F(Update, WhatIsRefusedAndWithWhichMessage) {
  struct Case {
    const char *body;
    const char *error;
  };
  const Case cases[] = {
      {R"({"network_type":"satellite"})", "'network_type' must be 'auto', 'wifi' or 'ethernet'"},
      {R"({"network_type":2})", "'network_type' must be 'auto', 'wifi' or 'ethernet'"},
      {R"({"wifi_networks":{"ssid":"home"}})", "'wifi_networks' must be an array"},
      {R"({"wifi_networks":[{"ssid":"1"},{"ssid":"2"},{"ssid":"3"},{"ssid":"4"},{"ssid":"5"},{"ssid":"6"}]})",
       "'wifi_networks' holds at most 5 entries"},
      {R"({"wifi_networks":["home"]})", "Each entry of 'wifi_networks' must be an object"},
      {R"({"wifi_networks":[{}]})", "'ssid' must be 1 to 32 characters"},
      {R"({"wifi_networks":[{"ssid":""}]})", "'ssid' must be 1 to 32 characters"},
      {R"({"wifi_networks":[{"ssid":123}]})", "'ssid' must be 1 to 32 characters"},
      {R"({"wifi_networks":[{"ssid":"home","password":"short7c"}]})", "'password' must be empty or 8 to 64 characters"},
      {R"({"wifi_networks":[{"ssid":"home","password":true}]})", "'password' must be empty or 8 to 64 characters"},
      {R"({"wifi_networks":[{"ssid":"home"},{"ssid":"home"}]})", "Duplicate SSID in 'wifi_networks'"},
      {R"({"wifi":[]})", "'wifi' must be an object"},
      {R"({"ethernet":"dhcp"})", "'ethernet' must be an object"},
      {R"({"wifi":{"use_dhcp":"yes"}})", "'use_dhcp' must be a boolean"},
      {R"({"wifi":{"manual_ip":[]}})", "'manual_ip' must be an object"},
      {R"({"wifi":{"manual_ip":{"static_ip":"010.0.0.1"}}})", "'manual_ip.static_ip' is not an IPv4 address"},
      {R"({"wifi":{"manual_ip":{"gateway":"10.0.0"}}})", "'manual_ip.gateway' is not an IPv4 address"},
      {R"({"wifi":{"manual_ip":{"subnet":"::1"}}})", "'manual_ip.subnet' is not an IPv4 address"},
      {R"({"wifi":{"manual_ip":{"dns1":"1.2.3.4.5"}}})", "'manual_ip.dns1' is not an IPv4 address"},
      {R"({"wifi":{"manual_ip":{"dns2":42}}})", "'manual_ip.dns2' is not an IPv4 address"},
      {R"({"ethernet":{"use_dhcp":false}})", "'manual_ip' needs 'static_ip', 'gateway' and 'subnet' without DHCP"},
      {R"({"ethernet":{"use_dhcp":false,"manual_ip":{"static_ip":"10.0.0.7","gateway":"10.0.0.1"}}})",
       "'manual_ip' needs 'static_ip', 'gateway' and 'subnet' without DHCP"},
  };

  const std::string before = this->report();
  for (const auto &test : cases) {
    EXPECT_STREQ(this->update(test.body), test.error) << test.body;
    // Nothing is written until everything passes.
    EXPECT_EQ(this->report(), before) << test.body;
    EXPECT_FALSE(this->settings.is_dirty()) << test.body;
  }
}

TEST_F(Update, ABodyThatIsNotAnObjectIsRefused) {
  JsonDocument doc;
  ASSERT_EQ(deserializeJson(doc, "[1,2]"), DeserializationError::Ok);
  EXPECT_STREQ(this->settings.update_from_json(doc.as<JsonObject>()), "Invalid JSON");
}

TEST_F(Update, AFirmwareWithoutWiFiRefusesEveryWiFiKey) {
  this->settings.set_available_interfaces(false, true);
  EXPECT_STREQ(this->update(R"({"wifi_networks":[{"ssid":"home"}]})"), "This firmware has no WiFi");
  EXPECT_STREQ(this->update(R"({"wifi":{"use_dhcp":true}})"), "This firmware has no WiFi");
  EXPECT_STREQ(this->update(R"({"network_type":"wifi"})"), "'network_type' is not available on this firmware");
  EXPECT_EQ(this->update(R"({"network_type":"auto"})"), nullptr);
}

TEST_F(Update, AFirmwareWithoutEthernetRefusesEveryEthernetKey) {
  this->settings.set_available_interfaces(true, false);
  EXPECT_STREQ(this->update(R"({"ethernet":{"use_dhcp":true}})"), "This firmware has no Ethernet");
  EXPECT_STREQ(this->update(R"({"network_type":"ethernet"})"), "'network_type' is not available on this firmware");
}

TEST_F(Update, ThePublicViewNamesThePasswordsWithoutGivingThemAway) {
  const std::string body = this->public_json();
  EXPECT_EQ(body.find("s3cret00"), std::string::npos) << body;
  EXPECT_NE(body.find(R"({"ssid":"home","has_password":true})"), std::string::npos) << body;
  EXPECT_NE(body.find(R"({"ssid":"shop","has_password":false})"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("network_type":"ethernet")"), std::string::npos) << body;
  EXPECT_NE(body.find(R"("static_ip":"192.168.1.50")"), std::string::npos) << body;
}

TEST_F(Update, ThePublicViewLeavesOutAnInterfaceTheFirmwareHasNot) {
  this->settings.set_available_interfaces(false, true);
  const std::string body = this->public_json();
  EXPECT_EQ(body.find("wifi"), std::string::npos) << body;
  EXPECT_NE(body.find("ethernet"), std::string::npos) << body;
}

TEST_F(Update, WhatThePublicViewAnswersIsWhatAPostTakesBack) {
  JsonDocument doc;
  this->settings.write_public_json(doc.to<JsonObject>());
  std::string body;
  serializeJson(doc, body);

  const std::string before = this->report();
  ASSERT_EQ(this->update(body), nullptr);
  EXPECT_EQ(this->report(), before);
}

}  // namespace esphome::network_config::testing
