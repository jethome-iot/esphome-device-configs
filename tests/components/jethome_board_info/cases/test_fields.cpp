// The field helpers: bounded-string extraction, MAC formatting, the exact string production
// signs, and the USID/serial cross-check that ties a record serial to the signed USID.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

#include "jeefs_vectors.h"

namespace esphome::jethome_board_info::testing {
namespace {

const char *field_at(const std::array<uint8_t, 256> &raw, size_t off) {
  return reinterpret_cast<const char *>(raw.data() + off);
}

// USID v2: {model:6}{hwrev:2}{pfrev:2}{yymm:4}{site:2}{serial:10}{checksum:4}.
std::string usid_with_serial(const std::string &serial_field) { return "JXD6E10100260101" + serial_field + "ABCD"; }

TEST(JeefsFields, FieldToStringStopsAtTheTerminator) {
  EXPECT_EQ(field_to_string(field_at(V4_HEADER_MINIMAL, HDR_OFF_BOARDNAME), 32), "JetHub-D2");
  EXPECT_EQ(field_to_string(field_at(V4_HEADER_MINIMAL, HDR_OFF_BOARDVERSION), 32), "2.1");
  EXPECT_EQ(field_to_string(field_at(V4_HEADER_MINIMAL, HDR_OFF_BOARD_SERIAL), 32), "BS-2026-00001");
  EXPECT_EQ(field_to_string(field_at(DEVID_RECORD_V1, REC_OFF_DEVICE_SERIAL), 32), "DSN-2026-000042");
  EXPECT_EQ(field_to_string(field_at(DEVID_RECORD_V1, REC_OFF_HW_REVISION), 16), "1.2a");
}

// A bounded string may fill its field with no NUL, which is why the length is capped, not searched.
TEST(JeefsFields, FieldToStringReadsAFullFieldWithNoTerminator) {
  const std::string board_serial = field_to_string(field_at(V4_HEADER_MAXLEN, HDR_OFF_BOARD_SERIAL), 32);
  EXPECT_EQ(board_serial, "BSERIAL-901234567890123456789012");
  EXPECT_EQ(board_serial.size(), 32u);
  EXPECT_EQ(field_to_string(field_at(V4_HEADER_MAXLEN, HDR_OFF_USID), 32), "USID-678901234567890123456789012");
  EXPECT_EQ(field_to_string(field_at(V4_HEADER_MAXLEN, HDR_OFF_CPUID), 32), "CPUID-78901234567890123456789012");
  EXPECT_EQ(field_to_string(field_at(V4_HEADER_MAXLEN, HDR_OFF_BOARDNAME), 32).size(), 31u);
}

TEST(JeefsFields, FieldToStringOnAnEmptyFieldIsAnEmptyString) {
  const std::array<char, 32> zeros{};
  EXPECT_EQ(field_to_string(zeros.data(), zeros.size()), "");
  EXPECT_EQ(field_to_string(field_at(V4_HEADER_MINIMAL, HDR_OFF_SIGNATURE), 64), "");
}

TEST(JeefsFields, FieldToStringIgnoresWhateverFollowsAnEmbeddedZero) {
  std::array<char, 32> field{};
  memcpy(field.data(), "abc\0\0xyz", 8);
  EXPECT_EQ(field_to_string(field.data(), field.size()), "abc");
  EXPECT_EQ(field_to_string(field.data(), 2), "ab");
  EXPECT_EQ(field_to_string(field.data(), 0), "");
}

TEST(JeefsFields, FormatMacIsUppercaseAndZeroPadded) {
  const std::array<uint8_t, 6> mac{0x00, 0x0A, 0x0B, 0xC0, 0x01, 0xFF};
  EXPECT_EQ(format_mac(mac.data()), "00:0A:0B:C0:01:FF");
  EXPECT_EQ(format_mac_plain(mac.data()), "000A0BC001FF");
}

TEST(JeefsFields, FormatMacMatchesTheUpstreamVectors) {
  EXPECT_EQ(format_mac(V4_HEADER_MINIMAL.data() + HDR_OFF_MAC), "F0:57:8D:10:20:30");
  EXPECT_EQ(format_mac_plain(V4_HEADER_MINIMAL.data() + HDR_OFF_MAC), "F0578D102030");
  EXPECT_EQ(format_mac(V4_HEADER_MAXLEN.data() + HDR_OFF_MAC), "F0:57:8D:AA:BB:CC");
  EXPECT_EQ(format_mac_plain(V3_HEADER_SECP256R1.data() + HDR_OFF_MAC), "F0578DAABBCC");
}

TEST(JeefsFields, FormatMacHandlesTheAllZeroAndAllOnesMacs) {
  const std::array<uint8_t, 6> zeros{};
  std::array<uint8_t, 6> ones{};
  ones.fill(0xFF);
  EXPECT_EQ(format_mac(zeros.data()), "00:00:00:00:00:00");
  EXPECT_EQ(format_mac_plain(zeros.data()), "000000000000");
  EXPECT_EQ(format_mac(ones.data()), "FF:FF:FF:FF:FF:FF");
  EXPECT_EQ(format_mac_plain(ones.data()), "FFFFFFFFFFFF");
}

TEST(JeefsFields, FormatMacProducesFixedWidthOutput) {
  const std::array<uint8_t, 6> mac{0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  EXPECT_EQ(format_mac(mac.data()).size(), 17u);
  EXPECT_EQ(format_mac_plain(mac.data()).size(), 12u);
}

TEST(JeefsFields, SigningPayloadIsColonSeparated) {
  EXPECT_EQ(signing_payload("cpu", "mac", "usid"), "cpu:mac:usid");
  EXPECT_EQ(signing_payload("", "", ""), "::");
}

// Byte-identical to what verify_signature_() builds: format_mac(factory), format_mac_plain(custom), usid.
TEST(JeefsFields, SigningPayloadMatchesWhatProductionBuilds) {
  const std::array<uint8_t, 6> factory{0xF0, 0x57, 0x8D, 0x10, 0x20, 0x30};
  const std::array<uint8_t, 6> custom{0xF0, 0x57, 0x8D, 0x10, 0x20, 0x31};
  const std::string usid = field_to_string(field_at(V4_HEADER_MAXLEN, HDR_OFF_USID), 32);

  const std::string payload = signing_payload(format_mac(factory.data()), format_mac_plain(custom.data()), usid);
  EXPECT_EQ(payload, "F0:57:8D:10:20:30:F0578D102031:USID-678901234567890123456789012");
}

TEST(JeefsFields, UsidEncodesASerialThatNeedsZeroPadding) {
  EXPECT_TRUE(usid_encodes_serial(usid_with_serial("0000012345"), "12345"));
  EXPECT_TRUE(usid_encodes_serial(usid_with_serial("0000000001"), "1"));
  EXPECT_TRUE(usid_encodes_serial(usid_with_serial("0000000000"), "0"));
}

TEST(JeefsFields, UsidEncodesASerialThatIsAlreadyTenDigits) {
  EXPECT_TRUE(usid_encodes_serial(usid_with_serial("1234567890"), "1234567890"));
  EXPECT_TRUE(usid_encodes_serial(usid_with_serial("2026000042"), "DSN-2026-000042"));
}

// Only the digits count, so an alphanumeric serial collapses to its digit run.
TEST(JeefsFields, UsidEncodesTheDigitsOfAnAlphanumericSerial) {
  EXPECT_TRUE(usid_encodes_serial(usid_with_serial("0006100001"), "JXD6E1ETH00001"));
  EXPECT_FALSE(usid_encodes_serial(usid_with_serial("0000000001"), "JXD6E1ETH00001"));
}

TEST(JeefsFields, UsidKeepsTheLastTenOfALongerDigitRun) {
  EXPECT_TRUE(usid_encodes_serial(usid_with_serial("5678901234"), "12345678901234"));
  EXPECT_FALSE(usid_encodes_serial(usid_with_serial("1234567890"), "12345678901234"));
}

TEST(JeefsFields, UsidRejectsASerialWithNoDigits) {
  EXPECT_FALSE(usid_encodes_serial(usid_with_serial("0000000000"), "ABCDEF"));
  EXPECT_FALSE(usid_encodes_serial(usid_with_serial("0000000000"), ""));
  EXPECT_FALSE(usid_encodes_serial(usid_with_serial("0000000000"), "-.-"));
}

// USID v2 is exactly 30 characters, and the length guard is load-bearing: without it the
// std::string::compare(16, 10, ...) below throws out_of_range on a shorter USID.
TEST(JeefsFields, UsidRejectsAnythingButThirtyCharacters) {
  const std::string good = usid_with_serial("0000012345");
  ASSERT_EQ(good.size(), USID_V2_LENGTH);
  EXPECT_FALSE(usid_encodes_serial("", "12345"));
  EXPECT_FALSE(usid_encodes_serial(good.substr(0, 29), "12345"));
  EXPECT_FALSE(usid_encodes_serial(good + "X", "12345"));
  EXPECT_FALSE(usid_encodes_serial(field_to_string(field_at(V4_HEADER_MINIMAL, HDR_OFF_USID), 32), "12345"));
}

TEST(JeefsFields, UsidRejectsAGenuineMismatch) {
  EXPECT_FALSE(usid_encodes_serial(usid_with_serial("0000012345"), "12346"));
  EXPECT_FALSE(usid_encodes_serial(usid_with_serial("0000012345"), "123450"));
}

// The serial lives at offset 16, so the same digits anywhere else do not satisfy the check.
TEST(JeefsFields, UsidReadsTheSerialAtItsOwnOffset) {
  EXPECT_FALSE(usid_encodes_serial("0000012345JXD6E101002601010101", "12345"));
}

}  // namespace
}  // namespace esphome::jethome_board_info::testing
