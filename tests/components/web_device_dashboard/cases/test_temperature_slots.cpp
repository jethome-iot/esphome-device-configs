#include "common.h"

namespace esphome::web_device_dashboard::testing {

static const char *const SLOTS = "/api/device/temperature-slots";
static const char *const FORGET = "/api/device/temperature-slots/forget";
static const char *const ASSIGN = "/api/device/temperature-slots/assign";
static const char *const CAPABILITIES = "/api/device/capabilities";

static const uint64_t ROM_A = 0xeb01227905460228ULL;
static const uint64_t ROM_B = 0x8a0122791699dd28ULL;
static const uint64_t ROM_C = 0x9b01b5566e8a1f28ULL;

// A YAML sensor for sensors:. Registered once: App keeps the pointer.
static sensor::Sensor &boiler() {
  static sensor::Sensor *instance = [] {
    auto *s = new sensor::Sensor();  // NOLINT(cppcoreguidelines-owning-memory)
    App.register_sensor(s, "Boiler", fnv1_hash("boiler"), 0);
    return s;
  }();
  return *instance;
}

// The dashboard wired to a scan of four slots. A boot is a new scan over the same bus and the
// same flash, so a case can forget and see what the next boot binds.
class TemperatureSlots : public Dashboard {
 protected:
  // @p listed gives slot 1 to a YAML sensor; @p listed_rom makes it a 1-Wire one with that device.
  TestScan &boot(std::vector<uint64_t> devices, bool listed = false, uint64_t listed_rom = 0) {
    this->bus.set_devices(std::move(devices));
    auto scan = std::make_unique<TestScan>();
    scan->set_one_wire_bus(&this->bus);
    scan->set_max_sensors(4);
    scan->set_preference_hash(fnv1_hash("temps"));
    if (listed)
      scan->set_sensor(0, &boiler());  // not a 1-Wire sensor unless it is pinned below
    if (listed_rom != 0)
      scan->pin(0, listed_rom);
    scan->setup();
    this->dashboard->set_temperature_slots(scan.get());
    this->boots.push_back(std::move(scan));
    return *this->boots.back();
  }

  // The confirmation with @p selector spliced in: R"("slot":1)" or R"("all":true)".
  std::string confirmed(const std::string &selector, const char *token = nullptr) {
    std::string body = this->confirmation(token);
    return body.substr(0, body.size() - 1) + "," + selector + "}";
  }

  one_wire_host::HostOneWireBus bus;
  std::vector<std::unique_ptr<TestScan>> boots;
};

// --- what the page is told ---

TEST_F(TemperatureSlots, CapabilitiesSayWhetherTheFirmwareHasSlots) {
  EXPECT_TRUE(this->get(CAPABILITIES)["temperature_slots"].isUnbound());
  this->boot({ROM_A});
  EXPECT_TRUE(this->get(CAPABILITIES)["temperature_slots"].as<bool>());
}

TEST_F(TemperatureSlots, WithoutAScanBothRoutesAreNotFound) {
  Reply list = this->get(SLOTS);
  EXPECT_EQ(list.code, 404);
  EXPECT_EQ(list.error(), "No temperature slots");
  Reply forget = this->post(FORGET, this->confirmed(R"("all":true)"));
  EXPECT_EQ(forget.code, 404);
  EXPECT_EQ(forget.error(), "No temperature slots");
}

TEST_F(TemperatureSlots, ListsEverySlotUpToTheLastBoundOne) {
  this->boot({ROM_A, ROM_B}, true);
  this->boot({ROM_B}, true).forget(1);  // A unplugged and forgotten: slot 2 is free
  this->boot({ROM_B}, true);

  Reply reply = this->get(SLOTS);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.type, "application/json");
  EXPECT_EQ(reply["max_slots"].as<int>(), 4);
  JsonArray slots = reply["slots"].as<JsonArray>();
  ASSERT_EQ(slots.size(), 3u);

  // The listed sensor: its own name, no address, not to be forgotten from here.
  EXPECT_EQ(slots[0]["slot"].as<int>(), 1);
  EXPECT_EQ(slots[0]["name"].as<std::string>(), "Boiler");
  EXPECT_FALSE(slots[0]["free"].as<bool>());
  EXPECT_TRUE(slots[0]["listed"].as<bool>());
  EXPECT_TRUE(slots[0]["address"].isUnbound());
  EXPECT_FALSE(slots[0]["can_forget"].as<bool>());

  // The freed slot keeps its row, under the name its sensor had.
  EXPECT_EQ(slots[1]["slot"].as<int>(), 2);
  EXPECT_EQ(slots[1]["name"].as<std::string>(), "Temp 2");
  EXPECT_TRUE(slots[1]["free"].as<bool>());
  EXPECT_FALSE(slots[1]["listed"].as<bool>());
  EXPECT_TRUE(slots[1]["address"].isUnbound());
  EXPECT_FALSE(slots[1]["can_forget"].as<bool>());

  EXPECT_EQ(slots[2]["slot"].as<int>(), 3);
  EXPECT_EQ(slots[2]["name"].as<std::string>(), "Temp 3");
  EXPECT_FALSE(slots[2]["free"].as<bool>());
  EXPECT_FALSE(slots[2]["listed"].as<bool>());
  // All sixteen digits, as the panel prints it: a ROM does not fit a JavaScript number.
  EXPECT_EQ(slots[2]["address"].as<std::string>(), "0x8a0122791699dd28");
  EXPECT_TRUE(slots[2]["can_forget"].as<bool>());
}

TEST_F(TemperatureSlots, AnEmptyTableListsNoSlots) {
  this->boot({});
  Reply reply = this->get(SLOTS);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply["max_slots"].as<int>(), 4);
  EXPECT_EQ(reply["slots"].as<JsonArray>().size(), 0u);
}

// --- forgetting ---

TEST_F(TemperatureSlots, ForgetASlotAnswersThenForgetsItAndReboots) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  Reply reply = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Forgetting slot 1, rebooting");
  // Still serving: the answer has to leave the socket first.
  EXPECT_EQ(scan.restarts, 0);
  this->loop();
  EXPECT_EQ(scan.restarts, 1);
  // The next boot finds slot 1 free, and B where it was.
  TestScan &after = this->boot({ROM_B});
  EXPECT_EQ(after.address(0), 0u);
  EXPECT_EQ(after.address(1), ROM_B);
}

TEST_F(TemperatureSlots, ForgetAllEmptiesEverySlotButTheListedOne) {
  this->boot({ROM_B}, true);
  TestScan &scan = this->boot({ROM_A, ROM_B}, true);
  ASSERT_EQ(scan.address(1), ROM_B);
  ASSERT_EQ(scan.address(2), ROM_A);
  Reply reply = this->post(FORGET, this->confirmed(R"("all":true)"));
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Forgetting every slot, rebooting");
  this->loop();
  EXPECT_EQ(scan.restarts, 1);
  // Numbered again in bus order, after the listed slot.
  TestScan &after = this->boot({ROM_A, ROM_B}, true);
  EXPECT_EQ(after.sensor(0), &boiler());
  EXPECT_EQ(after.address(1), ROM_A);
  EXPECT_EQ(after.address(2), ROM_B);
}

TEST_F(TemperatureSlots, ForgetRefusesWhatWouldChangeNothing) {
  TestScan &scan = this->boot({ROM_A}, true);
  Reply listed = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(listed.code, 409);
  EXPECT_EQ(listed.error(), "Slot 1 belongs to a sensor listed in YAML");
  Reply free = this->post(FORGET, this->confirmed(R"("slot":3)"));
  EXPECT_EQ(free.code, 409);
  EXPECT_EQ(free.error(), "Slot 3 is free");
  this->loop();
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(TemperatureSlots, ForgetAllRefusesATableWithOnlyTheListedSlot) {
  TestScan &scan = this->boot({}, true);
  Reply reply = this->post(FORGET, this->confirmed(R"("all":true)"));
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "Nothing to forget: every slot is free or listed in YAML");
  this->loop();
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(TemperatureSlots, ForgetNeedsOneSlotInRangeOrAll) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  Reply neither = this->post(FORGET, this->confirmation());
  EXPECT_EQ(neither.code, 400);
  EXPECT_EQ(neither.error(), "'slot' or 'all' is required");
  // Present is what counts, not the value: with "all": false, or a null, the request still names
  // both.
  for (const char *selector : {R"("slot":1,"all":true)", R"("slot":1,"all":false)", R"("slot":null,"all":true)",
                               R"("slot":null,"all":false)", R"("slot":1,"all":null)"}) {
    Reply both = this->post(FORGET, this->confirmed(selector));
    EXPECT_EQ(both.code, 400) << selector;
    EXPECT_EQ(both.error(), "'slot' and 'all' exclude each other") << selector;
  }
  // Numbered from 1, as the sensor names are; only a JSON integer is a slot.
  for (const char *selector : {R"("slot":0)", R"("slot":5)", R"("slot":-1)", R"("slot":"1")", R"("slot":1.5)",
                               R"("slot":true)", R"("slot":null)"}) {
    Reply reply = this->post(FORGET, this->confirmed(selector));
    EXPECT_EQ(reply.code, 400) << selector;
    EXPECT_EQ(reply.error(), "'slot' must be a number from 1 to 4") << selector;
  }
  // Only a JSON true asks for every slot, as only one confirms.
  for (const char *selector : {R"("all":1)", R"("all":false)", R"("all":"true")", R"("all":null)"}) {
    Reply reply = this->post(FORGET, this->confirmed(selector));
    EXPECT_EQ(reply.code, 400) << selector;
    EXPECT_EQ(reply.error(), "'all' must be true") << selector;
  }
  this->loop();
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(TemperatureSlots, ForgetTakesTheConfirmationTheSystemActionsTake) {
  TestScan &scan = this->boot({ROM_A});
  Reply unconfirmed = this->post(FORGET, R"({"slot":1})");
  EXPECT_EQ(unconfirmed.code, 400);
  EXPECT_EQ(unconfirmed.error(), "'confirm' must be true");
  Reply other_device = this->post(FORGET, this->confirmed(R"("slot":1)", "AA:BB:CC"));
  EXPECT_EQ(other_device.code, 403);
  Reply form = this->call(HTTP_POST, FORGET, this->confirmed(R"("slot":1)"), 512, "text/plain");
  EXPECT_EQ(form.code, 415);
  Reply cross_site =
      this->call(HTTP_POST, FORGET, this->confirmed(R"("slot":1)"), 512, "application/json", "http://evil.example");
  EXPECT_EQ(cross_site.code, 403);
  EXPECT_EQ(cross_site.body, "Cross-origin request refused");
  this->loop();
  EXPECT_EQ(scan.restarts, 0);
}

// --- assigning ---

// A table kept in a file whose partition did not mount: neither write could be kept, so neither
// answers with a reboot.
TEST_F(TemperatureSlots, WritesAreUnavailableWhenTheTableCannotBeSaved) {
  static dir_storage::DirStorage storage;  // never set up: not mounted
  static config_json::ConfigJsonKeeper keeper;
  keeper.set_storage(&storage);
  keeper.setup();
  ASSERT_FALSE(keeper.can_save());
  this->bus.set_devices({ROM_A});
  auto owned = std::make_unique<TestScan>();
  TestScan &scan = *owned;
  scan.set_one_wire_bus(&this->bus);
  scan.set_max_sensors(4);
  scan.set_slot_file(&keeper, "dallas_scan_temps");
  scan.setup();
  this->dashboard->set_temperature_slots(&scan);
  this->boots.push_back(std::move(owned));
  ASSERT_EQ(scan.address(0), ROM_A);

  Reply forget = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(forget.code, 503);
  EXPECT_EQ(forget.error(), "Temperature slot storage unavailable");
  Reply assign = this->post(ASSIGN, this->confirmed(R"("slot":2,"address":"0x9b01b5566e8a1f28")"));
  EXPECT_EQ(assign.code, 503);
  EXPECT_EQ(assign.error(), "Temperature slot storage unavailable");
  // The confirmation still comes first.
  EXPECT_EQ(this->post(FORGET, R"({"slot":1})").code, 400);
  this->loop();
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(TemperatureSlots, AssignAnswersThenSwapsAndReboots) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  Reply reply = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x8a0122791699dd28")"));
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Assigning slot 1, rebooting");
  EXPECT_EQ(scan.restarts, 0);
  this->loop();
  EXPECT_EQ(scan.restarts, 1);
  TestScan &after = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(after.address(0), ROM_B);
  EXPECT_EQ(after.address(1), ROM_A);
}

TEST_F(TemperatureSlots, AssignTakesANewAddressInEitherCaseWithOrWithoutThePrefix) {
  for (const char *address : {"0x9b01b5566e8a1f28", "0X9B01B5566E8A1F28", "9b01b5566e8a1f28"}) {
    // A table without C, for each spelling.
    global_preferences->reset();
    TestScan &scan = this->boot({ROM_A});
    Reply reply = this->post(ASSIGN, this->confirmed(std::string(R"("slot":3,"address":")") + address + "\""));
    EXPECT_EQ(reply.code, 200) << address << ": " << reply.error();
    this->loop();
    EXPECT_EQ(scan.restarts, 1) << address;
    EXPECT_EQ(this->boot({ROM_A}).address(2), ROM_C) << address;
  }
}

TEST_F(TemperatureSlots, AssignRefusesWhatIsNotARomAddress) {
  TestScan &scan = this->boot({ROM_A});
  for (const char *address : {R"("")", R"("0x")", R"("0x9b01b5566e8a1f2")", R"("0x9b01b5566e8a1f288")",
                              R"("0x9b01b5566e8a1fzz")", R"("0x 9b01b5566e8a1f2")", "1234", "null"}) {
    Reply reply = this->post(ASSIGN, this->confirmed(std::string(R"("slot":2,"address":)") + address));
    EXPECT_EQ(reply.code, 400) << address;
    EXPECT_EQ(reply.error(), "'address' must be 16 hex digits, after an optional 0x") << address;
  }
  Reply missing = this->post(ASSIGN, this->confirmed(R"("slot":2)"));
  EXPECT_EQ(missing.error(), "'address' must be 16 hex digits, after an optional 0x");
  // Well formed, but no thermometer has it: a serial number chip, and a CRC one bit off.
  Reply serial = this->post(ASSIGN, this->confirmed(R"("slot":2,"address":"0x4e00001234567801")"));
  EXPECT_EQ(serial.code, 400);
  EXPECT_EQ(serial.error(), "0x4e00001234567801 is not a thermometer ROM: wrong family or CRC");
  Reply crc = this->post(ASSIGN, this->confirmed(R"("slot":2,"address":"0x9a01b5566e8a1f28")"));
  EXPECT_EQ(crc.code, 400);
  EXPECT_EQ(crc.error(), "0x9a01b5566e8a1f28 is not a thermometer ROM: wrong family or CRC");
  this->loop();
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(TemperatureSlots, AssignNeedsASlotInRange) {
  TestScan &scan = this->boot({ROM_A});
  for (const char *slot : {"0", "5", R"("1")", "null"}) {
    Reply reply = this->post(ASSIGN, this->confirmed(std::string(R"("address":"0x9b01b5566e8a1f28","slot":)") + slot));
    EXPECT_EQ(reply.code, 400) << slot;
    EXPECT_EQ(reply.error(), "'slot' must be a number from 1 to 4") << slot;
  }
  this->loop();
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(TemperatureSlots, AssignRefusesWhatWouldChangeNothingOrFightTheYaml) {
  TestScan &scan = this->boot({ROM_C, ROM_A}, true, ROM_C);
  ASSERT_EQ(scan.address(1), ROM_A);
  Reply listed_slot = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0xeb01227905460228")"));
  EXPECT_EQ(listed_slot.code, 409);
  EXPECT_EQ(listed_slot.error(), "Slot 1 belongs to a sensor listed in YAML");
  Reply listed_rom = this->post(ASSIGN, this->confirmed(R"("slot":3,"address":"0x9b01b5566e8a1f28")"));
  EXPECT_EQ(listed_rom.code, 409);
  EXPECT_EQ(listed_rom.error(), "0x9b01b5566e8a1f28 belongs to a sensor listed in YAML");
  Reply unchanged = this->post(ASSIGN, this->confirmed(R"("slot":2,"address":"0xEB01227905460228")"));
  EXPECT_EQ(unchanged.code, 409);
  EXPECT_EQ(unchanged.error(), "0xeb01227905460228 is in slot 2 already");
  this->loop();
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(TemperatureSlots, AssignTakesTheConfirmationTheSystemActionsTake) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  const char *selector = R"("slot":1,"address":"0x8a0122791699dd28")";
  Reply unconfirmed = this->post(ASSIGN, R"({"slot":1,"address":"0x8a0122791699dd28"})");
  EXPECT_EQ(unconfirmed.code, 400);
  EXPECT_EQ(unconfirmed.error(), "'confirm' must be true");
  Reply other_device = this->post(ASSIGN, this->confirmed(selector, "AA:BB:CC"));
  EXPECT_EQ(other_device.code, 403);
  Reply form = this->call(HTTP_POST, ASSIGN, this->confirmed(selector), 512, "text/plain");
  EXPECT_EQ(form.code, 415);
  Reply cross_site =
      this->call(HTTP_POST, ASSIGN, this->confirmed(selector), 512, "application/json", "http://evil.example");
  EXPECT_EQ(cross_site.code, 403);
  this->loop();
  EXPECT_EQ(scan.restarts, 0);
}

TEST_F(TemperatureSlots, WithoutAScanAssignIsNotFound) {
  Reply reply = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x8a0122791699dd28")"));
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "No temperature slots");
}

}  // namespace esphome::web_device_dashboard::testing
