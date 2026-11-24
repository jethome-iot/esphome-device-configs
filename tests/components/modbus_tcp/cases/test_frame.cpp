#include "common.h"

namespace esphome::modbus_tcp::testing {

// A read of six coils from 0x0000, as transaction 1 to unit 1.
static const Bytes READ_COILS = {0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x01, 0x01, 0x00, 0x00, 0x00, 0x06};

TEST(Frame, WaitsForTheHeader) {
  for (size_t size = 0; size < MBAP_HEADER_SIZE; size++) {
    const Frame frame = find_frame(std::span<const uint8_t>(READ_COILS.data(), size));
    EXPECT_EQ(frame.status, FrameStatus::INCOMPLETE) << size;
  }
}

TEST(Frame, WaitsForTheWholeRequest) {
  for (size_t size = MBAP_HEADER_SIZE; size < READ_COILS.size(); size++) {
    const Frame frame = find_frame(std::span<const uint8_t>(READ_COILS.data(), size));
    EXPECT_EQ(frame.status, FrameStatus::INCOMPLETE) << size;
  }
  const Frame frame = find_frame(READ_COILS);
  EXPECT_EQ(frame.status, FrameStatus::COMPLETE);
  EXPECT_EQ(frame.size, READ_COILS.size());
}

// Two requests in one read: the first ends where its length says, the rest waits its turn.
TEST(Frame, EndsWhereTheLengthSays) {
  Bytes stream = READ_COILS;
  stream.insert(stream.end(), READ_COILS.begin(), READ_COILS.begin() + 9);
  const Frame frame = find_frame(stream);
  EXPECT_EQ(frame.status, FrameStatus::COMPLETE);
  EXPECT_EQ(frame.size, READ_COILS.size());
}

TEST(Frame, RefusesAnotherProtocol) {
  Bytes adu = READ_COILS;
  adu[3] = 0x01;
  EXPECT_EQ(find_frame(adu).status, FrameStatus::INVALID);
}

// The length counts the unit id and the PDU: at least a function code, at most a 253-byte PDU.
TEST(Frame, RefusesALengthOutOfRange) {
  for (uint16_t length : {0, 1, 255, 256, 0xFFFF}) {
    Bytes adu = READ_COILS;
    adu[4] = static_cast<uint8_t>(length >> 8);
    adu[5] = static_cast<uint8_t>(length);
    EXPECT_EQ(find_frame(adu).status, FrameStatus::INVALID) << length;
  }
  Bytes adu(MAX_ADU_SIZE, 0x00);
  adu[5] = 254;
  const Frame frame = find_frame(adu);
  EXPECT_EQ(frame.status, FrameStatus::COMPLETE);
  EXPECT_EQ(frame.size, MAX_ADU_SIZE);
}

}  // namespace esphome::modbus_tcp::testing
