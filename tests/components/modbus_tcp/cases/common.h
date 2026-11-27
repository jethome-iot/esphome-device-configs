#pragma once

#include <gtest/gtest.h>

#include "esphome/components/modbus_server/modbus_server.h"
#include "esphome/components/modbus_tcp/mbap.h"

#include <array>
#include <memory>
#include <vector>

namespace esphome::modbus_tcp::testing {

using Bytes = std::vector<uint8_t>;
using modbus::helpers::SensorValueType;
using modbus_server::ModbusServer;
using modbus_server::ServerBit;
using modbus_server::ServerRegister;

/// The relay board's server as features/modbus-server.yaml builds it: relays as coils 0x0000-0x0005,
/// any of which a thermostat can hold; inputs as discrete inputs 0x0010-0x0015;
/// temperatures as S_WORD registers 0x0000-0x000F; unmapped registers read 0. A writable U_WORD at
/// 0x0020 stands in for a register the board does not have, so the register writes are reachable.
class Server : public ::testing::Test {
 protected:
  void SetUp() override {
    for (uint16_t i = 0; i < 6; i++) {
      auto &bit = this->bits_.emplace_back(std::make_unique<ServerBit>(i));
      bit->set_read_lambda([this, i](uint16_t) -> optional<bool> { return this->relays[i]; });
      bit->set_write_lambda([this, i](uint16_t, bool value) {
        if (i == this->held_relay && value != this->relays[i])
          return false;
        this->relays[i] = value;
        return true;
      });
      this->server.add_server_bit(bit.get());
    }
    for (uint16_t i = 0; i < 6; i++) {
      auto &bit = this->bits_.emplace_back(std::make_unique<ServerBit>(0x0010 + i));
      bit->set_read_lambda([this, i](uint16_t) -> optional<bool> { return this->inputs[i]; });
      this->server.add_server_bit(bit.get());
    }
    for (uint16_t i = 0; i < 16; i++) {
      auto &reg = this->registers_.emplace_back(std::make_unique<ServerRegister>(i, SensorValueType::S_WORD, 1));
      reg->set_read_lambda<int16_t>([this, i](uint16_t) -> optional<int16_t> { return this->temperatures[i]; });
      this->server.add_server_register(reg.get());
    }
    auto &reg = this->registers_.emplace_back(std::make_unique<ServerRegister>(0x0020, SensorValueType::U_WORD, 1));
    reg->set_read_lambda<uint16_t>([this](uint16_t) -> optional<uint16_t> { return this->setpoint; });
    reg->set_write_lambda<uint16_t>([this](uint16_t, uint16_t value) {
      this->setpoint = value;
      return true;
    });
    this->server.add_server_register(reg.get());
    this->server.set_server_courtesy_response({.enabled = true, .register_last_address = 0xFFFF, .register_value = 0});
  }

  /// The response PDU to @p pdu, sent to @p unit as transaction 0x1234. The MBAP header that comes
  /// back is checked here: the transaction, protocol and unit as sent, the length as answered.
  Bytes request(const Bytes &pdu, uint8_t unit = 1) {
    Bytes adu = {
        0x12, 0x34, 0x00, 0x00, static_cast<uint8_t>((pdu.size() + 1) >> 8), static_cast<uint8_t>(pdu.size() + 1),
        unit};
    adu.insert(adu.end(), pdu.begin(), pdu.end());
    const Frame frame = find_frame(adu);
    EXPECT_EQ(frame.status, FrameStatus::COMPLETE);
    EXPECT_EQ(frame.size, adu.size());
    std::array<uint8_t, MAX_ADU_SIZE> out{};
    const size_t len = answer_adu(this->server, this->unit_id, adu, out);
    EXPECT_GE(len, MBAP_HEADER_SIZE + 2);
    EXPECT_EQ(out[0], 0x12);
    EXPECT_EQ(out[1], 0x34);
    EXPECT_EQ(out[2], 0x00);
    EXPECT_EQ(out[3], 0x00);
    EXPECT_EQ((out[4] << 8) | out[5], len - 6);
    EXPECT_EQ(out[6], unit);
    return Bytes(out.begin() + MBAP_HEADER_SIZE, out.begin() + len);
  }

  ModbusServer server;
  uint8_t unit_id{1};
  std::array<bool, 6> relays{};
  std::array<bool, 6> inputs{};
  std::array<int16_t, 16> temperatures{};
  uint16_t setpoint{0};
  // The relay a running thermostat drives, by index; -1 for none.
  int held_relay{-1};

 private:
  std::vector<std::unique_ptr<ServerBit>> bits_;
  std::vector<std::unique_ptr<ServerRegister>> registers_;
};

}  // namespace esphome::modbus_tcp::testing
