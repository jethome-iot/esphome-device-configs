#include "common.h"
#include "esphome/components/modbus_map/modbus_map.h"

namespace esphome::web_device_dashboard::testing {

static const char *const CAPABILITIES = "/api/device/capabilities";

// What a client parses: the key re-serialized, so the comparison covers the order, the types and
// every key that should not be there.
static std::string modbus_of(Reply &reply) {
  std::string out;
  serializeJson(reply["modbus"], out);
  return out;
}

TEST_F(Dashboard, CapabilitiesLeaveModbusOutWithoutAMap) {
  Reply reply = this->get(CAPABILITIES);
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply["modbus"].isUnbound()) << reply.body;
}

// The relay board's map, as modbus-server.yaml derives it.
TEST_F(Dashboard, CapabilitiesDescribeTheModbusMap) {
  modbus_map::ModbusMap map;
  map.add_bits({.address = 0x0000, .last_address = 0x0005, .count = 6, .writable = true, .name = "Relays"});
  map.add_bits({.address = 0x0010, .last_address = 0x0015, .count = 6, .writable = false, .name = "Inputs"});
  map.add_registers({.address = 0x0000,
                     .last_address = 0x000F,
                     .count = 16,
                     .writable = false,
                     .name = "Temperature slots",
                     .value_type = "S_WORD",
                     .scale = 0.1f,
                     .unit = "°C",
                     .no_value = 0x8000});
  map.set_courtesy_response(0xFFFF, 0);
  this->dashboard->set_modbus_map(&map);

  Reply reply = this->get(CAPABILITIES);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(modbus_of(reply), R"({"bits":[)"
                              R"({"address":0,"last_address":5,"count":6,"writable":true,"name":"Relays"},)"
                              R"({"address":16,"last_address":21,"count":6,"writable":false,"name":"Inputs"}],)"
                              R"("registers":[{"address":0,"last_address":15,"count":16,"writable":false,)"
                              R"("name":"Temperature slots","value_type":"S_WORD","scale":0.1,"unit":"°C",)"
                              R"("no_value":32768}],)"
                              R"("courtesy_response":{"last_address":65535,"value":0}})");
  // The scale as the map gives it, not the float's nearest double.
  EXPECT_NE(reply.body.find(R"("scale":0.1,)"), std::string::npos) << reply.body;
}

TEST_F(Dashboard, CapabilitiesLeaveOutWhatTheMapDoesNotGive) {
  modbus_map::ModbusMap map;
  map.add_registers({.address = 0x0000,
                     .last_address = 0xFFFF,
                     .count = 65536,
                     .writable = true,
                     .name = "Everything",
                     .value_type = "U_WORD"});
  this->dashboard->set_modbus_map(&map);

  Reply reply = this->get(CAPABILITIES);
  // No scale, unit or no_value, no courtesy response; the empty bit table is still there.
  EXPECT_EQ(modbus_of(reply), R"({"bits":[],"registers":[{"address":0,"last_address":65535,"count":65536,)"
                              R"("writable":true,"name":"Everything","value_type":"U_WORD"}]})");
}

TEST_F(Dashboard, CapabilitiesCarryANoValueOfZero) {
  // 0 is a word like any other; only "not given" leaves the key out.
  modbus_map::ModbusMap map;
  map.add_registers({.address = 0x0100,
                     .last_address = 0x0100,
                     .count = 1,
                     .writable = false,
                     .name = "Level",
                     .value_type = "U_WORD",
                     .unit = "%",
                     .no_value = 0});
  map.set_courtesy_response(0x01FF, 0xFFFF);
  this->dashboard->set_modbus_map(&map);

  Reply reply = this->get(CAPABILITIES);
  EXPECT_EQ(modbus_of(reply), R"({"bits":[],"registers":[{"address":256,"last_address":256,"count":1,)"
                              R"("writable":false,"name":"Level","value_type":"U_WORD","unit":"%","no_value":0}],)"
                              R"("courtesy_response":{"last_address":511,"value":65535}})");
}

TEST_F(Dashboard, CapabilitiesDescribeAMapWithNoRanges) {
  modbus_map::ModbusMap map;
  this->dashboard->set_modbus_map(&map);
  Reply reply = this->get(CAPABILITIES);
  EXPECT_EQ(modbus_of(reply), R"({"bits":[],"registers":[]})");
}

}  // namespace esphome::web_device_dashboard::testing
