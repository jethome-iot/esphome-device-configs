#include <sys/stat.h>
#include <unistd.h>
#include <fstream>
#include <iterator>
#include "common.h"

namespace esphome::network_config::testing {

static const char *const FULL =
    R"({"version":1,"network_type":"auto",)"
    R"("wifi":{"use_dhcp":false,"manual_ip":{"static_ip":"192.168.1.50","gateway":"192.168.1.1",)"
    R"("subnet":"255.255.255.0","dns1":"192.168.1.1","dns2":""}},)"
    R"("ethernet":{"use_dhcp":true,"manual_ip":{"static_ip":"","gateway":"","subnet":"","dns1":"","dns2":""}},)"
    R"("wifi_networks":[{"ssid":"home","password":"s3cret00"},{"ssid":"shop","password":""}]})";

TEST_F(Record, StartsOnEthernetWithNothingConfigured) {
  EXPECT_EQ(this->settings.network_type(), NetworkType::ETHERNET);
  EXPECT_TRUE(this->settings.networks().empty());
  EXPECT_TRUE(this->settings.wifi().use_dhcp);
  EXPECT_TRUE(this->settings.ethernet().use_dhcp);
  EXPECT_FALSE(this->settings.is_broken());
}

TEST_F(Record, ReadsBackWhatItWrote) {
  ASSERT_TRUE(this->load(FULL));
  EXPECT_EQ(this->settings.network_type(), NetworkType::AUTO);
  ASSERT_EQ(this->settings.networks().size(), 2U);
  EXPECT_EQ(this->settings.networks()[0].ssid, "home");
  EXPECT_EQ(this->settings.networks()[0].password, "s3cret00");
  EXPECT_FALSE(this->settings.wifi().use_dhcp);
  EXPECT_EQ(this->settings.wifi().manual_ip.gateway, "192.168.1.1");
  EXPECT_EQ(this->file(), FULL);
}

TEST_F(Record, KeepsThePasswordsInTheClear) {
  // The file is plain, and config/ stays readable over the file API: the README says so.
  ASSERT_TRUE(this->load(FULL));
  EXPECT_NE(this->file().find("s3cret00"), std::string::npos);
}

TEST_F(Record, TakesTheDefaultsForEveryKeyTheFileLeavesOut) {
  ASSERT_TRUE(this->load(R"({"network_type":"wifi"})"));
  EXPECT_EQ(this->settings.network_type(), NetworkType::WIFI);
  EXPECT_TRUE(this->settings.wifi().use_dhcp);
  EXPECT_TRUE(this->settings.networks().empty());
}

TEST_F(Record, ADocumentWithNoneOfTheKeysIsBroken) {
  EXPECT_FALSE(this->load(R"({"version":1})"));
  EXPECT_TRUE(this->settings.is_broken());
  EXPECT_EQ(this->settings.network_type(), NetworkType::ETHERNET);
  EXPECT_TRUE(log().has_error("none of the expected keys"));
}

TEST_F(Record, AnUpdateClearsTheBrokenFlag) {
  ASSERT_FALSE(this->load(R"({"version":1})"));
  ASSERT_EQ(this->update(R"({"network_type":"auto"})"), nullptr);
  EXPECT_FALSE(this->settings.is_broken());
}

TEST_F(Record, AnUnknownModeFallsBackToEthernet) {
  ASSERT_TRUE(this->load(R"({"network_type":"satellite"})"));
  EXPECT_EQ(this->settings.network_type(), NetworkType::ETHERNET);
  EXPECT_TRUE(log().has_warning("Unknown network_type"));
}

TEST_F(Record, DropsOnlyTheUnusableEntriesOfTheList) {
  ASSERT_TRUE(this->load(R"({"wifi_networks":[{"ssid":"a"},{"password":"x"},{"ssid":""},{"ssid":"b"}]})"));
  ASSERT_EQ(this->settings.networks().size(), 2U);
  EXPECT_EQ(this->settings.networks()[0].ssid, "a");
  EXPECT_EQ(this->settings.networks()[1].ssid, "b");
  EXPECT_TRUE(log().has_warning("Dropping"));
}

TEST_F(Record, KeepsAtMostFiveNetworksFromTheFile) {
  ASSERT_TRUE(this->load(R"({"wifi_networks":[{"ssid":"1"},{"ssid":"2"},{"ssid":"3"},)"
                         R"({"ssid":"4"},{"ssid":"5"},{"ssid":"6"}]})"));
  EXPECT_EQ(this->settings.networks().size(), MAX_NETWORKS);
  EXPECT_TRUE(log().has_warning("more than 5 WiFi networks"));
}

TEST_F(Record, AnIncompleteStaticBlockInTheFileFallsBackToDhcp) {
  ASSERT_TRUE(this->load(R"({"ethernet":{"use_dhcp":false,"manual_ip":{"static_ip":"10.0.0.7"}}})"));
  EXPECT_TRUE(this->settings.ethernet().use_dhcp);
  EXPECT_EQ(this->settings.ethernet().manual_ip.static_ip, "10.0.0.7");
  EXPECT_TRUE(log().has_warning("falling back to DHCP"));
}

TEST_F(Record, ABadAddressInTheFileIsDroppedNotKept) {
  ASSERT_TRUE(this->load(R"({"wifi":{"use_dhcp":true,"manual_ip":{"static_ip":"010.0.0.1","gateway":"10.0.0.1"}}})"));
  EXPECT_EQ(this->settings.wifi().manual_ip.static_ip, "");
  EXPECT_EQ(this->settings.wifi().manual_ip.gateway, "10.0.0.1");
  EXPECT_TRUE(log().has_warning("is not an IPv4 address"));
}

TEST_F(Record, AnInterfaceOfTheWrongTypeIsIgnored) {
  ASSERT_TRUE(this->load(R"({"wifi":"static","wifi_networks":[]})"));
  EXPECT_TRUE(this->settings.wifi().use_dhcp);
  EXPECT_TRUE(log().has_warning("is not an object"));
}

TEST_F(Record, ResetGoesBackToTheCompiledDefaults) {
  ASSERT_TRUE(this->load(FULL));
  this->settings.reset();
  EXPECT_EQ(this->report(), "ethernet wifi=dhcp eth=dhcp");
  EXPECT_TRUE(this->settings.is_dirty());
}

TEST_F(Record, AdoptsUnknownCredentialsAsThePrimaryNetwork) {
  ASSERT_TRUE(this->load(FULL));
  EXPECT_TRUE(this->settings.adopt_network("portal", "hunter22"));
  ASSERT_EQ(this->settings.networks().size(), 3U);
  EXPECT_EQ(this->settings.networks()[0].ssid, "portal");
  EXPECT_EQ(this->settings.networks()[1].ssid, "home");
}

TEST_F(Record, AdoptingWhatIsAlreadyPrimaryChangesNothing) {
  ASSERT_TRUE(this->load(FULL));
  this->settings.clear_dirty();
  EXPECT_FALSE(this->settings.adopt_network("home", "s3cret00"));
  EXPECT_FALSE(this->settings.is_dirty());
}

TEST_F(Record, AdoptingAKnownSsidMovesItUpAndTakesTheNewPassword) {
  ASSERT_TRUE(this->load(FULL));
  EXPECT_TRUE(this->settings.adopt_network("shop", "newpass1"));
  ASSERT_EQ(this->settings.networks().size(), 2U);
  EXPECT_EQ(this->settings.networks()[0].ssid, "shop");
  EXPECT_EQ(this->settings.networks()[0].password, "newpass1");
}

TEST_F(Record, AdoptingKeepsTheListWithinTheCap) {
  ASSERT_TRUE(this->load(R"({"wifi_networks":[{"ssid":"1"},{"ssid":"2"},{"ssid":"3"},{"ssid":"4"},{"ssid":"5"}]})"));
  EXPECT_TRUE(this->settings.adopt_network("portal", ""));
  EXPECT_EQ(this->settings.networks().size(), MAX_NETWORKS);
  EXPECT_EQ(this->settings.networks()[0].ssid, "portal");
  EXPECT_EQ(this->settings.networks().back().ssid, "4");
}

TEST_F(Record, AdoptingRefusesAnEmptyOrOversizedSsid) {
  EXPECT_FALSE(this->settings.adopt_network("", "whatever"));
  EXPECT_FALSE(this->settings.adopt_network(std::string(33, 'x'), ""));
  EXPECT_TRUE(this->settings.networks().empty());
}

TEST_F(Record, TheModeAndTheListAreTheLiveHalfOfTheFingerprint) {
  ASSERT_TRUE(this->load(FULL));
  const std::string live = this->settings.live_fingerprint();
  const std::string boot = this->settings.boot_fingerprint();

  ASSERT_EQ(this->update(R"({"network_type":"wifi"})"), nullptr);
  EXPECT_NE(this->settings.live_fingerprint(), live);
  EXPECT_EQ(this->settings.boot_fingerprint(), boot);

  ASSERT_EQ(this->update(R"({"ethernet":{"use_dhcp":false,"manual_ip":{"static_ip":"10.0.0.7",)"
                         R"("gateway":"10.0.0.1","subnet":"255.255.255.0"}}})"),
            nullptr);
  EXPECT_NE(this->settings.boot_fingerprint(), boot);
}

// The file over a real directory, the way the keeper runs it over the partition.
class Storage : public Record {
 protected:
  void SetUp() override {
    Record::SetUp();
    mkdir(".storage", 0755);
    char folder[] = ".storage/XXXXXX";
    ASSERT_NE(mkdtemp(folder), nullptr);
    this->backend.path = folder;
  }

  void TearDown() override {
    remove(this->path().c_str());
    rmdir((this->backend.path + "/config").c_str());
    rmdir(this->backend.path.c_str());
  }

  std::string path() const { return this->backend.path + "/config/network.json"; }
  void write(const std::string &text) {
    mkdir((this->backend.path + "/config").c_str(), 0755);
    std::ofstream out(this->path());
    out << text;
  }
  std::string read() {
    std::ifstream in(this->path());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }

  FakeStorage backend;
};

TEST_F(Storage, NoFileMeansTheDefaultsAndNoComplaint) {
  EXPECT_TRUE(this->settings.load_from_file(&this->backend, "config"));
  EXPECT_EQ(this->report(), "ethernet wifi=dhcp eth=dhcp");
  EXPECT_FALSE(this->settings.is_broken());
  EXPECT_TRUE(log().errors.empty());
}

TEST_F(Storage, SurvivesASaveAndALoad) {
  ASSERT_EQ(this->update(R"({"network_type":"auto","wifi_networks":[{"ssid":"home","password":"s3cret00"}],)"
                         R"("ethernet":{"use_dhcp":false,"manual_ip":{"static_ip":"10.0.0.7",)"
                         R"("gateway":"10.0.0.1","subnet":"255.255.255.0"}}})"),
            nullptr);
  mkdir((this->backend.path + "/config").c_str(), 0755);
  ASSERT_TRUE(this->settings.save_to_file(&this->backend, "config"));
  EXPECT_FALSE(this->settings.is_dirty());

  NetworkSettingsJson second;
  second.set_available_interfaces(true, true);
  ASSERT_TRUE(second.load_from_file(&this->backend, "config"));
  EXPECT_EQ(second.network_type(), NetworkType::AUTO);
  ASSERT_EQ(second.networks().size(), 1U);
  EXPECT_EQ(second.networks()[0].password, "s3cret00");
  EXPECT_EQ(second.ethernet().manual_ip.static_ip, "10.0.0.7");
}

TEST_F(Storage, LeavesADamagedFileAloneAndStartsFromTheDefaults) {
  this->write("{not json");
  EXPECT_FALSE(this->settings.load_from_file(&this->backend, "config"));
  EXPECT_EQ(this->report(), "ethernet wifi=dhcp eth=dhcp");
  // Nothing became dirty, so the keeper has nothing to write over it with.
  EXPECT_FALSE(this->settings.is_dirty());
  EXPECT_EQ(this->read(), "{not json");
}

}  // namespace esphome::network_config::testing
