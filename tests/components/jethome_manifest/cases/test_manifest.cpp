#include "common.h"

namespace esphome::jethome_manifest::testing {

TEST(Manifest, TakesTheChannelSlot) {
  update::UpdateInfo info;
  ASSERT_TRUE(parse(MANIFEST, "release", info));
  EXPECT_EQ(info.latest_version, "2026.8.2.0");
  EXPECT_EQ(info.md5, "8fd271ebf20c980fac0168646e8fa600");
  EXPECT_EQ(info.firmware_url, "https://fw.jethome.com/media/release-ota.bin");
  EXPECT_EQ(info.title, "JXD-R6-E1ETH-LCD");
  EXPECT_EQ(info.release_url, "https://fw.jethome.com/devices/jethome/jxd-r6-e1eth-lcd/fw/release/CHANGELOG.md");
}

TEST(Manifest, TakesTheOtaImageOfTheOtherChannel) {
  update::UpdateInfo info;
  ASSERT_TRUE(parse(MANIFEST, "nightly", info));
  EXPECT_EQ(info.latest_version, "2026.8.2.20260917.1");
  EXPECT_EQ(info.md5, "448059cb4a86188fa325956e3b69f09f");
  EXPECT_EQ(info.firmware_url, "https://fw.jethome.com/media/nightly-ota.bin");
  // Not every channel serves a changelog.
  EXPECT_TRUE(info.release_url.empty());
}

TEST(Manifest, RefusesAChannelTheServerDoesNotCarry) {
  update::UpdateInfo info;
  EXPECT_FALSE(parse(MANIFEST, "beta", info));
  EXPECT_TRUE(log().has_error("no 'beta' firmware"));
  EXPECT_TRUE(info.latest_version.empty());
}

TEST(Manifest, MatchesTheWholeLastSlotComponent) {
  const std::string manifest = R"({"latest_firmware": {"firmware.esphome.jxd.prerelease": {
      "version": "1.0.0", "images": {"esp.ota": {"url": "/a.bin", "hash": "8fd271ebf20c980fac0168646e8fa600"}}}}})";
  update::UpdateInfo info;
  EXPECT_FALSE(parse(manifest, "release", info));
}

TEST(Manifest, MatchesASlotNamedAfterTheChannelAlone) {
  const std::string manifest = R"({"latest_firmware": {"nightly": {
      "version": "1.0.0", "images": {"esp.ota": {"url": "/a.bin", "hash": "8fd271ebf20c980fac0168646e8fa600"}}}}})";
  update::UpdateInfo info;
  ASSERT_TRUE(parse(manifest, "nightly", info));
  EXPECT_EQ(info.latest_version, "1.0.0");
}

TEST(Manifest, RefusesASlotWithoutAnOtaImage) {
  const std::string manifest = R"({"latest_firmware": {"fw.release": {
      "version": "1.0.0", "images": {"esp.bin": {"url": "/a.bin", "hash": "8fd271ebf20c980fac0168646e8fa600"}}}}})";
  update::UpdateInfo info;
  EXPECT_FALSE(parse(manifest, "release", info));
  EXPECT_TRUE(log().has_error("no esp.ota image"));
}

TEST(Manifest, RefusesASlotWithoutAVersion) {
  const std::string manifest = R"({"latest_firmware": {"fw.release": {
      "images": {"esp.ota": {"url": "/a.bin", "hash": "8fd271ebf20c980fac0168646e8fa600"}}}}})";
  update::UpdateInfo info;
  EXPECT_FALSE(parse(manifest, "release", info));
}

// A sha256 there would otherwise fail the update halfway through the download.
TEST(Manifest, RefusesAHashThatIsNotAnMd5) {
  for (const char *hash : {"448059cb4a86188fa325956e3b69f09f448059cb4a86188fa325956e3b69f09f", "", "not-a-hash"}) {
    const std::string manifest = std::string(R"({"latest_firmware": {"fw.release": {"version": "1.0.0",
        "images": {"esp.ota": {"url": "/a.bin", "hash": ")") +
                                 hash + R"("}}}}})";
    update::UpdateInfo info;
    EXPECT_FALSE(parse(manifest, "release", info)) << hash;
    EXPECT_TRUE(log().has_error("is not an md5")) << hash;
    EXPECT_TRUE(info.firmware_url.empty()) << hash;
  }
}

// The OTA component only ever computes a lowercase digest to compare against.
TEST(Manifest, LowercasesAnUppercaseHash) {
  const std::string manifest = R"({"latest_firmware": {"fw.release": {"version": "1.0.0",
      "images": {"esp.ota": {"url": "/a.bin", "hash": "8FD271EBF20C980FAC0168646E8FA600"}}}}})";
  update::UpdateInfo info;
  ASSERT_TRUE(parse(manifest, "release", info));
  EXPECT_EQ(info.md5, "8fd271ebf20c980fac0168646e8fa600");
}

TEST(Manifest, LeavesTheTitleEmptyWhenTheManifestNamesNoDevice) {
  const std::string manifest = R"({"latest_firmware": {"fw.release": {"version": "1.0.0",
      "images": {"esp.ota": {"url": "/a.bin", "hash": "8fd271ebf20c980fac0168646e8fa600"}}}}})";
  update::UpdateInfo info;
  ASSERT_TRUE(parse(manifest, "release", info));
  EXPECT_TRUE(info.title.empty());
}

TEST(Manifest, RefusesAnEmptyVersionOrUrl) {
  for (const char *fields : {R"("version": "", "images": {"esp.ota": {"url": "/a.bin",)",
                             R"("version": "1.0.0", "images": {"esp.ota": {"url": "",)"}) {
    const std::string manifest = std::string(R"({"latest_firmware": {"fw.release": {)") + fields +
                                 R"( "hash": "8fd271ebf20c980fac0168646e8fa600"}}}}})";
    update::UpdateInfo info;
    EXPECT_FALSE(parse(manifest, "release", info)) << fields;
    EXPECT_TRUE(info.latest_version.empty()) << fields;
  }
}

TEST(Manifest, RefusesAManifestWithoutFirmware) {
  update::UpdateInfo info;
  EXPECT_FALSE(parse(R"({"device": "jxd-r6-e1eth-lcd"})", "release", info));
  EXPECT_TRUE(log().has_error("no latest_firmware"));
}

TEST(Manifest, RefusesWhatIsNotJson) {
  update::UpdateInfo info;
  EXPECT_FALSE(parse("<html>404</html>", "release", info));
}

TEST(Url, KeepsAnAbsoluteUrl) {
  EXPECT_EQ(resolve_url(SOURCE, "https://fw.jethome.com/a.bin"), "https://fw.jethome.com/a.bin");
  EXPECT_EQ(resolve_url(SOURCE, "http://other.example/a.bin"), "http://other.example/a.bin");
}

TEST(Url, TakesTheServerForAnAbsolutePath) {
  EXPECT_EQ(resolve_url(SOURCE, "/media/a.bin"), "https://fw.jethome.com/media/a.bin");
}

TEST(Url, TakesTheManifestsDirectoryForARelativePath) {
  EXPECT_EQ(resolve_url(SOURCE, "a.bin"), "https://fw.jethome.com/api/devices/jxd-r6-e1eth-lcd/a.bin");
}

TEST(Url, TakesTheSchemeForASchemeRelativeUrl) {
  EXPECT_EQ(resolve_url(SOURCE, "//cdn.example/a.bin"), "https://cdn.example/a.bin");
}

TEST(Url, TakesTheServerOfASourceThatHasNoPath) {
  EXPECT_EQ(resolve_url("https://fw.jethome.com", "/a.bin"), "https://fw.jethome.com/a.bin");
}

TEST(Url, TakesTheServerItselfWhenTheSourceHasNoPath) {
  EXPECT_EQ(resolve_url("https://fw.jethome.com", "a.bin"), "https://fw.jethome.com/a.bin");
}

TEST(Url, IgnoresAQueryAndFragmentOfTheManifestUrl) {
  EXPECT_EQ(resolve_url("https://fw.jethome.com/api/info?next=/old", "a.bin"), "https://fw.jethome.com/api/a.bin");
  EXPECT_EQ(resolve_url("https://fw.jethome.com/api/info#top", "/a.bin"), "https://fw.jethome.com/a.bin");
}

TEST(Url, LeavesAnEmptyUrlAlone) { EXPECT_EQ(resolve_url(SOURCE, ""), ""); }

}  // namespace esphome::jethome_manifest::testing
