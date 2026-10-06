#include "common.h"
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <cmath>
#include <fstream>
#include <map>

namespace esphome::web_device_dashboard::testing {

static const char *const SLOTS = "/api/device/temperature-slots";
static const char *const FORGET = "/api/device/temperature-slots/forget";
static const char *const ASSIGN = "/api/device/temperature-slots/assign";
static const char *const OFFSET = "/api/device/temperature-slots/offset";
static const char *const CAPABILITIES = "/api/device/capabilities";
static const char *const STATUS = "/api/device/status";
static const char *const REBOOT = "/api/device/system/reboot";

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

// The harness bus, except that a device given a reading answers with a 12-bit DS18B20 scratch
// pad that passes its checksum, as in the dallas_scan suite.
class ReadingBus : public one_wire_host::HostOneWireBus {
 public:
  void set_reading(uint64_t address, float celsius) {
    const auto raw = (int16_t) std::lround(celsius * 16);
    std::array<uint8_t, 9> pad = {(uint8_t) (raw & 0xff), (uint8_t) (raw >> 8), 0x4b, 0x46, 0x7f, 0xff, 0x0c, 0x10, 0};
    pad[8] = crc8(pad.data(), 8);
    this->pads_[address] = pad;
  }

  void write64(uint64_t address) override {
    this->selected_ = address;
    this->next_ = 0;
  }
  uint8_t read8() override {
    auto pad = this->pads_.find(this->selected_);
    return pad == this->pads_.end() || this->next_ >= pad->second.size() ? 0xFF : pad->second[this->next_++];
  }

 protected:
  std::map<uint64_t, std::array<uint8_t, 9>> pads_;
  uint64_t selected_{0};
  size_t next_{0};
};

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

  // What the page's reboot notice reads: /status says a slot change waits, and why.
  bool waits() {
    Reply status = this->get(STATUS);
    EXPECT_EQ(status.code, 200);
    if (!status["reboot_required"].as<bool>()) {
      EXPECT_TRUE(status["reboot_reasons"].isUnbound()) << status.body;
      return false;
    }
    EXPECT_EQ(status["reboot_reasons"].size(), 1u) << status.body;
    EXPECT_EQ(status["reboot_reasons"][0].as<std::string>(), "temperature_slots") << status.body;
    return true;
  }

  // {"slot": N, "offset": X} as a POST body; @p offset is the JSON text, so a case can send any.
  Reply set_offset(int slot, const std::string &offset) {
    return this->post(OFFSET, R"({"slot":)" + std::to_string(slot) + R"(,"offset":)" + offset + "}");
  }

  ReadingBus bus;
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
  Reply offset = this->set_offset(1, "0.3");
  EXPECT_EQ(offset.code, 404);
  EXPECT_EQ(offset.error(), "No temperature slots");
}

TEST_F(TemperatureSlots, ListsEverySlotUpToTheLastBoundOne) {
  this->boot({ROM_A, ROM_B}, true);
  this->boot({ROM_B}, true).forget(1);  // A unplugged and forgotten: slot 2 is free
  this->boot({ROM_B}, true);

  Reply reply = this->get(SLOTS);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply.type, "application/json");
  EXPECT_EQ(reply["max_slots"].as<int>(), 4);
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
  JsonArray slots = reply["slots"].as<JsonArray>();
  ASSERT_EQ(slots.size(), 3u);
  // Just booted: every slot is what runs.
  for (JsonObject slot : slots) {
    EXPECT_TRUE(slot["pending"].is<bool>());
    EXPECT_FALSE(slot["pending"].as<bool>());
    EXPECT_TRUE(slot["running_address"].isUnbound());
  }

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
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
  EXPECT_EQ(reply["slots"].as<JsonArray>().size(), 0u);
}

// Rows describe the saved table, the one the next boot binds; `name` and `running_address` are
// what runs until then. Booted {A, B}, then A and B swapped and C put into slot 4.
TEST_F(TemperatureSlots, TheListShowsTheSavedTableNextToWhatRuns) {
  this->boot({ROM_A, ROM_B});
  ASSERT_EQ(this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x8a0122791699dd28")")).code, 200);
  ASSERT_EQ(this->post(ASSIGN, this->confirmed(R"("slot":4,"address":"0x9b01b5566e8a1f28")")).code, 200);

  Reply reply = this->get(SLOTS);
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  JsonArray slots = reply["slots"].as<JsonArray>();
  ASSERT_EQ(slots.size(), 4u) << reply.body;

  EXPECT_EQ(slots[0]["name"].as<std::string>(), "Temp 1");
  EXPECT_FALSE(slots[0]["free"].as<bool>());
  EXPECT_EQ(slots[0]["address"].as<std::string>(), "0x8a0122791699dd28");
  EXPECT_EQ(slots[0]["running_address"].as<std::string>(), "0xeb01227905460228");
  EXPECT_TRUE(slots[0]["pending"].as<bool>());
  EXPECT_TRUE(slots[0]["can_forget"].as<bool>());

  EXPECT_EQ(slots[1]["address"].as<std::string>(), "0xeb01227905460228");
  EXPECT_EQ(slots[1]["running_address"].as<std::string>(), "0x8a0122791699dd28");
  EXPECT_TRUE(slots[1]["pending"].as<bool>());

  // Not touched: no running_address, as on a fresh boot.
  EXPECT_EQ(slots[2]["name"].as<std::string>(), "Temp 3");
  EXPECT_TRUE(slots[2]["free"].as<bool>());
  EXPECT_FALSE(slots[2]["pending"].as<bool>());
  EXPECT_TRUE(slots[2]["address"].isUnbound());
  EXPECT_TRUE(slots[2]["running_address"].isUnbound());

  // Past the slots bound at boot, so listed for the saved table alone; nothing runs there yet.
  EXPECT_EQ(slots[3]["slot"].as<int>(), 4);
  EXPECT_EQ(slots[3]["name"].as<std::string>(), "Temp 4");
  EXPECT_FALSE(slots[3]["free"].as<bool>());
  EXPECT_EQ(slots[3]["address"].as<std::string>(), "0x9b01b5566e8a1f28");
  EXPECT_TRUE(slots[3]["pending"].as<bool>());
  EXPECT_TRUE(slots[3]["running_address"].isUnbound());
  EXPECT_TRUE(slots[3]["can_forget"].as<bool>());
}

// A forgotten slot with a device at boot: free in the saved table, still read until the reboot.
TEST_F(TemperatureSlots, AForgottenSlotIsFreeAndStillRunsItsDevice) {
  this->boot({ROM_A, ROM_B});
  ASSERT_EQ(this->post(FORGET, this->confirmed(R"("slot":1)")).code, 200);
  Reply list = this->get(SLOTS);
  JsonArray slots = list["slots"].as<JsonArray>();
  ASSERT_EQ(slots.size(), 2u);
  EXPECT_EQ(slots[0]["name"].as<std::string>(), "Temp 1");
  EXPECT_TRUE(slots[0]["free"].as<bool>());
  EXPECT_TRUE(slots[0]["address"].isUnbound());
  EXPECT_EQ(slots[0]["running_address"].as<std::string>(), "0xeb01227905460228");
  EXPECT_TRUE(slots[0]["pending"].as<bool>());
  EXPECT_FALSE(slots[0]["can_forget"].as<bool>());
  // Forgetting it again says the slot is free from the reboot on, not that it is free now.
  Reply again = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(again.code, 409);
  EXPECT_EQ(again.error(), "Slot 1 is free after a reboot");
}

// A move empties the slot it left as a forget does, so the refusal says the same.
TEST_F(TemperatureSlots, ASlotAMoveEmptiedIsFreeAfterAReboot) {
  this->boot({ROM_A});
  ASSERT_EQ(this->post(ASSIGN, this->confirmed(R"("slot":3,"address":"0xeb01227905460228")")).code, 200);
  Reply reply = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "Slot 1 is free after a reboot");
}

// --- forgetting ---

TEST_F(TemperatureSlots, ForgetASlotSavesTheTableAndWaitsForAReboot) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  EXPECT_FALSE(this->waits());
  Reply reply = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Slot 1 forgotten; applies after a reboot");
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(scan.restarts, 0);
  // /status says so, read where the request is: no job for the loop task.
  const int jobs = this->dashboard->jobs;
  EXPECT_TRUE(this->waits());
  EXPECT_EQ(this->dashboard->jobs, jobs);
  // The next boot finds slot 1 free, and B where it was; nothing waits then.
  TestScan &after = this->boot({ROM_B});
  EXPECT_EQ(after.address(0), 0u);
  EXPECT_EQ(after.address(1), ROM_B);
  EXPECT_FALSE(this->waits());
}

TEST_F(TemperatureSlots, ForgetAllEmptiesEverySlotButTheListedOne) {
  this->boot({ROM_B}, true);
  TestScan &scan = this->boot({ROM_A, ROM_B}, true);
  ASSERT_EQ(scan.address(1), ROM_B);
  ASSERT_EQ(scan.address(2), ROM_A);
  Reply reply = this->post(FORGET, this->confirmed(R"("all":true)"));
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Every slot forgotten; applies after a reboot");
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(scan.restarts, 0);
  // Numbered again in bus order, after the listed slot.
  TestScan &after = this->boot({ROM_A, ROM_B}, true);
  EXPECT_EQ(after.sensor(0), &boiler());
  EXPECT_EQ(after.address(1), ROM_A);
  EXPECT_EQ(after.address(2), ROM_B);
}

TEST_F(TemperatureSlots, ForgetRefusesWhatWouldChangeNothing) {
  this->boot({ROM_A}, true);
  Reply listed = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(listed.code, 409);
  EXPECT_EQ(listed.error(), "Slot 1 belongs to a sensor listed in YAML");
  Reply free = this->post(FORGET, this->confirmed(R"("slot":3)"));
  EXPECT_EQ(free.code, 409);
  EXPECT_EQ(free.error(), "Slot 3 is free");
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_FALSE(this->waits());
}

TEST_F(TemperatureSlots, ForgetAllRefusesATableWithOnlyTheListedSlot) {
  this->boot({}, true);
  Reply reply = this->post(FORGET, this->confirmed(R"("all":true)"));
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "Nothing to forget: every slot is free or listed in YAML");
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_FALSE(this->waits());
}

TEST_F(TemperatureSlots, ForgetNeedsOneSlotInRangeOrAll) {
  this->boot({ROM_A, ROM_B});
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
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_FALSE(this->waits());
}

TEST_F(TemperatureSlots, ForgetTakesTheConfirmationTheSystemActionsTake) {
  this->boot({ROM_A});
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
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_FALSE(this->waits());
}

// --- assigning ---

// A table kept in a file whose partition did not mount: neither write could be kept, so both
// are refused up front.
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
  // The list says so up front: no slot can be forgotten, nor all of them.
  Reply list = this->get(SLOTS);
  EXPECT_FALSE(list["slots"][0]["can_forget"].as<bool>());
  EXPECT_FALSE(list["can_forget_all"].as<bool>());

  Reply forget = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(forget.code, 503);
  EXPECT_EQ(forget.error(), "Temperature slot storage unavailable");
  Reply assign = this->post(ASSIGN, this->confirmed(R"("slot":2,"address":"0x9b01b5566e8a1f28")"));
  EXPECT_EQ(assign.code, 503);
  EXPECT_EQ(assign.error(), "Temperature slot storage unavailable");
  // Refused up front, the offset a slot holds already included.
  for (const char *value : {"0.3", "0"}) {
    Reply offset = this->set_offset(1, value);
    EXPECT_EQ(offset.code, 503) << value;
    EXPECT_EQ(offset.error(), "Temperature slot storage unavailable") << value;
  }
  EXPECT_EQ(scan.offset(0), 0.0f);
  // The confirmation still comes first, and a body that is not JSON.
  EXPECT_EQ(this->post(FORGET, R"({"slot":1})").code, 400);
  EXPECT_EQ(this->post(OFFSET, "slot=1").code, 400);
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_FALSE(this->waits());
}

// The write happens before the answer, so a table that could not be written is an error, and
// nothing waits for a reboot that would bring the old table back.
TEST_F(TemperatureSlots, AWriteThatFailsIsAnErrorAndTheDeviceKeepsRunning) {
  if (geteuid() == 0)
    GTEST_SKIP() << "root writes into a read-only folder";
  mkdir(".storage", 0755);
  char folder[] = ".storage/XXXXXX";
  ASSERT_NE(mkdtemp(folder), nullptr);
  static dir_storage::DirStorage storage;
  storage.set_base_path(folder);
  storage.setup();
  static config_json::ConfigJsonKeeper keeper;
  keeper.set_storage(&storage);
  keeper.setup();
  this->bus.set_devices({ROM_A, ROM_B});
  auto owned = std::make_unique<TestScan>();
  TestScan &scan = *owned;
  scan.set_one_wire_bus(&this->bus);
  scan.set_max_sensors(4);
  scan.set_slot_file(&keeper, "dallas_scan_temps");
  scan.setup();
  this->dashboard->set_temperature_slots(&scan);
  this->boots.push_back(std::move(owned));
  const std::string dir = std::string(folder) + "/config";
  ASSERT_EQ(chmod(dir.c_str(), 0555), 0);

  Reply forget = this->post(FORGET, this->confirmed(R"("slot":1)"));
  Reply assign = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x9b01b5566e8a1f28")"));
  Reply offset = this->set_offset(1, "0.3");
  chmod(dir.c_str(), 0755);
  EXPECT_EQ(forget.code, 500);
  EXPECT_EQ(forget.error(), "The slot table was not written");
  EXPECT_EQ(assign.code, 500);
  EXPECT_EQ(assign.error(), "The slot table was not written");
  EXPECT_EQ(offset.code, 500);
  EXPECT_EQ(offset.error(), "The offset was not written");
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(scan.saved_address(0), ROM_A);
  EXPECT_EQ(scan.saved_address(1), ROM_B);
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_EQ(this->get(SLOTS)["slots"][0]["offset"].as<float>(), 0.0f);
  EXPECT_FALSE(this->waits());
  remove((dir + "/dallas_scan_temps.json").c_str());
  rmdir(dir.c_str());
  rmdir(folder);
}

// The table is the loop task's: a busy loop answers for every route rather than reading or
// writing it from the server task.
TEST_F(TemperatureSlots, ABusyLoopIsUnavailableForEveryRoute) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  this->dashboard->loop_busy = true;
  Reply list = this->get(SLOTS);
  Reply forget = this->post(FORGET, this->confirmed(R"("slot":1)"));
  Reply assign = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x9b01b5566e8a1f28")"));
  Reply offset = this->set_offset(1, "0.3");
  this->dashboard->loop_busy = false;
  for (Reply *reply : {&list, &forget, &assign, &offset}) {
    EXPECT_EQ(reply->code, 503);
    EXPECT_EQ(reply->error(), "Device busy");
  }
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(scan.saved_address(0), ROM_A);
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_FALSE(this->waits());
}

// Every change is saved as it comes and the device keeps running; one reboot applies them all.
TEST_F(TemperatureSlots, SeveralChangesWaitForOneReboot) {
  this->boot({ROM_A, ROM_B, ROM_C});
  Reply first = this->post(FORGET, this->confirmed(R"("slot":3)"));
  ASSERT_EQ(first.code, 200) << first.error();
  EXPECT_EQ(this->get(SLOTS).code, 200);
  Reply swap = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x8a0122791699dd28")"));
  EXPECT_EQ(swap.code, 200) << swap.error();
  EXPECT_EQ(swap.message(), "Slot 1 assigned; applies after a reboot");
  Reply ahead = this->post(ASSIGN, this->confirmed(R"("slot":4,"address":"0x9b01b5566e8a1f28")"));
  EXPECT_EQ(ahead.code, 200) << ahead.error();
  EXPECT_TRUE(ahead["reboot_required"].as<bool>());
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_TRUE(this->waits());

  // "Reboot now" is the system route; the next boot binds all three changes.
  ASSERT_EQ(this->post(REBOOT, this->confirmation()).code, 200);
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 1);
  TestScan &after = this->boot({ROM_A, ROM_B, ROM_C});
  EXPECT_EQ(after.address(0), ROM_B);
  EXPECT_EQ(after.address(1), ROM_A);
  EXPECT_EQ(after.address(2), 0u);
  EXPECT_EQ(after.address(3), ROM_C);
  EXPECT_FALSE(this->waits());
}

// A change that puts the table back as booted leaves nothing for a reboot to do.
TEST_F(TemperatureSlots, UndoingAChangeLeavesNothingWaiting) {
  this->boot({ROM_A, ROM_B});
  ASSERT_EQ(this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x8a0122791699dd28")")).code, 200);
  EXPECT_TRUE(this->waits());
  Reply back = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0xeb01227905460228")"));
  EXPECT_EQ(back.code, 200) << back.error();
  EXPECT_EQ(back.message(), "Slot 1 assigned");
  EXPECT_FALSE(back["reboot_required"].as<bool>());
  EXPECT_FALSE(this->waits());
  Reply list = this->get(SLOTS);
  EXPECT_FALSE(list["reboot_required"].as<bool>());
  for (JsonObject slot : list["slots"].as<JsonArray>())
    EXPECT_FALSE(slot["pending"].as<bool>());

  // A forget undone by putting the device back: its answer says nothing waits.
  ASSERT_EQ(this->post(FORGET, this->confirmed(R"("slot":2)")).code, 200);
  EXPECT_TRUE(this->waits());
  Reply restored = this->post(ASSIGN, this->confirmed(R"("slot":2,"address":"0x8a0122791699dd28")"));
  EXPECT_EQ(restored.message(), "Slot 2 assigned");
  EXPECT_FALSE(this->waits());
}

// A forget can be the undo too: of an assign into a slot that was free at boot.
TEST_F(TemperatureSlots, AForgetThatLeavesTheTableAsBootedSaysNothingWaits) {
  this->boot({ROM_A});
  ASSERT_EQ(this->post(ASSIGN, this->confirmed(R"("slot":3,"address":"0x9b01b5566e8a1f28")")).code, 200);
  EXPECT_TRUE(this->waits());
  Reply forget = this->post(FORGET, this->confirmed(R"("slot":3)"));
  EXPECT_EQ(forget.code, 200) << forget.error();
  EXPECT_EQ(forget.message(), "Slot 3 forgotten");
  EXPECT_FALSE(forget["reboot_required"].as<bool>());
  EXPECT_FALSE(this->waits());
}

TEST_F(TemperatureSlots, AssignSwapsAndWaitsForAReboot) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  Reply reply = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x8a0122791699dd28")"));
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(reply.message(), "Slot 1 assigned; applies after a reboot");
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(scan.restarts, 0);
  EXPECT_TRUE(this->waits());
  TestScan &after = this->boot({ROM_A, ROM_B});
  EXPECT_EQ(after.address(0), ROM_B);
  EXPECT_EQ(after.address(1), ROM_A);
}

TEST_F(TemperatureSlots, AssignTakesANewAddressInEitherCaseWithOrWithoutThePrefix) {
  for (const char *address : {"0x9b01b5566e8a1f28", "0X9B01B5566E8A1F28", "9b01b5566e8a1f28"}) {
    // A table without C, for each spelling.
    global_preferences->reset();
    this->boot({ROM_A});
    Reply reply = this->post(ASSIGN, this->confirmed(std::string(R"("slot":3,"address":")") + address + "\""));
    EXPECT_EQ(reply.code, 200) << address << ": " << reply.error();
    this->loop();
    EXPECT_EQ(this->dashboard->restarts, 0) << address;
    EXPECT_EQ(this->boot({ROM_A}).address(2), ROM_C) << address;
  }
}

TEST_F(TemperatureSlots, AssignRefusesWhatIsNotARomAddress) {
  this->boot({ROM_A});
  const int jobs = this->dashboard->jobs;
  for (const char *address : {R"("")", R"("0x")", R"("0x9b01b5566e8a1f2")", R"("0x9b01b5566e8a1f288")",
                              R"("0x9b01b5566e8a1fzz")", R"("0x 9b01b5566e8a1f2")", "1234", "null",
                              // A NUL ends a C string, not a JSON one: the 16 digits before it are not
                              // all that was sent.
                              R"("9b01b5566e8a1f28\u0000junk")", R"("0x9b01b5566e8a1f2\u00008")"}) {
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
  // About the request alone: none of it waited for the loop task.
  EXPECT_EQ(this->dashboard->jobs, jobs);
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_FALSE(this->waits());
}

TEST_F(TemperatureSlots, AssignNeedsASlotInRange) {
  this->boot({ROM_A});
  for (const char *slot : {"0", "5", R"("1")", "null"}) {
    Reply reply = this->post(ASSIGN, this->confirmed(std::string(R"("address":"0x9b01b5566e8a1f28","slot":)") + slot));
    EXPECT_EQ(reply.code, 400) << slot;
    EXPECT_EQ(reply.error(), "'slot' must be a number from 1 to 4") << slot;
  }
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_FALSE(this->waits());
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
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_FALSE(this->waits());
}

TEST_F(TemperatureSlots, AssignTakesTheConfirmationTheSystemActionsTake) {
  this->boot({ROM_A, ROM_B});
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
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_FALSE(this->waits());
}

TEST_F(TemperatureSlots, WithoutAScanAssignIsNotFound) {
  Reply reply = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x8a0122791699dd28")"));
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "No temperature slots");
}

// --- offsets ---

// The range comes with the list, and every slot but a listed one carries its offset, 0 included.
TEST_F(TemperatureSlots, TheListCarriesTheRangeAndEveryUnlistedSlotsOffset) {
  this->boot({ROM_A, ROM_B}, true);
  ASSERT_EQ(this->set_offset(3, "-0.3").code, 200);
  Reply reply = this->get(SLOTS);
  ASSERT_EQ(reply.code, 200);
  EXPECT_EQ(reply["max_offset"].as<float>(), 5.0f);
  EXPECT_FLOAT_EQ(reply["offset_step"].as<float>(), 0.1f);
  JsonArray slots = reply["slots"].as<JsonArray>();
  ASSERT_EQ(slots.size(), 3u) << reply.body;
  EXPECT_TRUE(slots[0]["listed"].as<bool>());
  EXPECT_TRUE(slots[0]["offset"].isUnbound()) << reply.body;
  EXPECT_TRUE(slots[1]["offset"].is<float>());
  EXPECT_EQ(slots[1]["offset"].as<float>(), 0.0f);
  EXPECT_FLOAT_EQ(slots[2]["offset"].as<float>(), -0.3f);
  // As printed: the step and the offset to one decimal, not the double nearest the float.
  EXPECT_NE(reply.body.find(R"("max_offset":5,"offset_step":0.1,)"), std::string::npos) << reply.body;
  EXPECT_NE(reply.body.find(R"("offset":0})"), std::string::npos) << reply.body;
  EXPECT_NE(reply.body.find(R"("offset":-0.3})"), std::string::npos) << reply.body;
}

// The list runs to the last slot holding an offset too, so a free one past the sensors shows it.
TEST_F(TemperatureSlots, AFreeSlotHoldingAnOffsetIsListed) {
  this->boot({ROM_A});
  Reply set = this->set_offset(4, "1.5");
  ASSERT_EQ(set.code, 200) << set.error();
  Reply list = this->get(SLOTS);
  JsonArray slots = list["slots"].as<JsonArray>();
  ASSERT_EQ(slots.size(), 4u) << list.body;
  EXPECT_EQ(slots[1]["offset"].as<float>(), 0.0f);
  EXPECT_EQ(slots[3]["slot"].as<int>(), 4);
  EXPECT_EQ(slots[3]["name"].as<std::string>(), "Temp 4");
  EXPECT_TRUE(slots[3]["free"].as<bool>());
  EXPECT_TRUE(slots[3]["address"].isUnbound());
  EXPECT_FALSE(slots[3]["can_forget"].as<bool>());
  EXPECT_FALSE(slots[3]["pending"].as<bool>());
  EXPECT_FLOAT_EQ(slots[3]["offset"].as<float>(), 1.5f);
  // Nothing waits: an offset is in force already.
  EXPECT_FALSE(list["reboot_required"].as<bool>());
  EXPECT_FALSE(this->waits());
  // Back to 0, the row goes with it.
  ASSERT_EQ(this->set_offset(4, "0").code, 200);
  EXPECT_EQ(this->get(SLOTS)["slots"].as<JsonArray>().size(), 1u);
}

// Written and in force before the answer: the reading moves without a poll, and the next boot
// still has it.
TEST_F(TemperatureSlots, AnOffsetAppliesAtOnceAndStays) {
  this->bus.set_reading(ROM_A, 20.0f);
  TestScan &scan = this->boot({ROM_A, ROM_B});
  scan.poll();
  ASSERT_FLOAT_EQ(scan.sensor(0)->state, 20.0f);
  Reply reply = this->set_offset(1, "-0.3");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.type, "application/json");
  EXPECT_EQ(reply.body, R"({"success":true,"message":"Slot 1 offset -0.3 °C; applies now","offset":-0.3})");
  EXPECT_FLOAT_EQ(scan.sensor(0)->state, 19.7f);
  EXPECT_FLOAT_EQ(scan.offset(0), -0.3f);
  this->loop();
  EXPECT_EQ(this->dashboard->restarts, 0);
  EXPECT_EQ(scan.restarts, 0);
  EXPECT_FALSE(this->waits());

  TestScan &after = this->boot({ROM_A, ROM_B});
  EXPECT_FLOAT_EQ(after.offset(0), -0.3f);
  after.poll();
  EXPECT_FLOAT_EQ(after.sensor(0)->state, 19.7f);
  EXPECT_FLOAT_EQ(this->get(SLOTS)["slots"][0]["offset"].as<float>(), -0.3f);
}

// The answer is the offset now held, after the rounding to 0.1; an integer is a number too.
TEST_F(TemperatureSlots, AnOffsetIsRoundedToATenth) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  struct Case {
    const char *sent;
    const char *held;
    const char *message;
  };
  for (const Case &c : std::vector<Case>{{"0.25", "0.3", "+0.3"},
                                         {"-0.25", "-0.3", "-0.3"},
                                         {"0.04", "0", "0.0"},
                                         {"1", "1", "+1.0"},
                                         {"5", "5", "+5.0"},
                                         {"-5.04", "-5", "-5.0"},
                                         {"4.96e0", "5", "+5.0"},
                                         {"-0", "0", "0.0"}}) {
    Reply reply = this->set_offset(2, c.sent);
    EXPECT_EQ(reply.code, 200) << c.sent << ": " << reply.error();
    EXPECT_EQ(reply.message(), std::string("Slot 2 offset ") + c.message + " °C; applies now") << c.sent;
    EXPECT_NE(reply.body.find(std::string(R"("offset":)") + c.held + "}"), std::string::npos) << reply.body;
    EXPECT_FLOAT_EQ(reply["offset"].as<float>(), scan.offset(1)) << c.sent;
  }
}

// Nothing to write, nothing refused: the page may save a dialog whose offset it did not change.
TEST_F(TemperatureSlots, SettingTheSameOffsetAgainIsFine) {
  TestScan &scan = this->boot({ROM_A});
  for (int i = 0; i < 2; i++) {
    Reply reply = this->set_offset(1, "0.3");
    EXPECT_EQ(reply.code, 200) << reply.error();
    EXPECT_EQ(reply.message(), "Slot 1 offset +0.3 °C; applies now");
  }
  EXPECT_FLOAT_EQ(scan.offset(0), 0.3f);
  Reply zero = this->set_offset(3, "0");
  EXPECT_EQ(zero.code, 200);
  EXPECT_EQ(zero.body, R"({"success":true,"message":"Slot 3 offset 0.0 °C; applies now","offset":0})");
}

TEST_F(TemperatureSlots, AnOffsetNeedsASlotInRangeAndANumberWithinFiveDegrees) {
  TestScan &scan = this->boot({ROM_A});
  for (const char *body : {R"({"slot":0,"offset":0.3})", R"({"slot":5,"offset":0.3})", R"({"slot":"1","offset":0.3})",
                           R"({"slot":1.5,"offset":0.3})", R"({"slot":null,"offset":0.3})", R"({"offset":0.3})"}) {
    Reply reply = this->post(OFFSET, body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "'slot' must be a number from 1 to 4") << body;
  }
  // Past ±5.0 once rounded, far past what a float holds, and anything that is not a JSON number.
  for (const char *value : {"5.05", "-5.05", "5.1", "-10", "1e300", "-1e300", "1e999", R"("0.3")", "null", "true",
                            "[0.3]", R"({"value":0.3})"}) {
    Reply reply = this->set_offset(1, value);
    EXPECT_EQ(reply.code, 400) << value;
    EXPECT_EQ(reply.error(), "'offset' must be a number from -5.0 to 5.0") << value;
  }
  Reply missing = this->post(OFFSET, R"({"slot":1})");
  EXPECT_EQ(missing.code, 400);
  EXPECT_EQ(missing.error(), "'offset' must be a number from -5.0 to 5.0");
  for (const char *body : {"", "{", "[1,0.3]", "0.3"}) {
    Reply reply = this->post(OFFSET, body);
    EXPECT_EQ(reply.code, 400) << body;
    EXPECT_EQ(reply.error(), "Invalid JSON") << body;
  }
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_FALSE(this->waits());
}

TEST_F(TemperatureSlots, AListedSlotTakesNoOffset) {
  TestScan &scan = this->boot({ROM_A}, true);
  Reply reply = this->set_offset(1, "0.3");
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "Slot 1 belongs to a sensor listed in YAML");
  // The range is checked first, as dallas_scan checks it.
  EXPECT_EQ(this->set_offset(1, "6").code, 400);
  EXPECT_EQ(scan.offset(0), 0.0f);
}

// No confirmation, but every other guard a body-reading route has.
TEST_F(TemperatureSlots, AnOffsetTakesNoConfirmationButJsonFromThisSite) {
  TestScan &scan = this->boot({ROM_A});
  const int jobs = this->dashboard->jobs;
  Reply form = this->call(HTTP_POST, OFFSET, R"({"slot":1,"offset":0.3})", 512, "text/plain");
  EXPECT_EQ(form.code, 415);
  Reply cross_site =
      this->call(HTTP_POST, OFFSET, R"({"slot":1,"offset":0.3})", 512, "application/json", "http://evil.example");
  EXPECT_EQ(cross_site.code, 403);
  Reply large = this->post(OFFSET, R"({"slot":1,"offset":0.3,"pad":")" + std::string(5000, 'x') + R"("})");
  EXPECT_EQ(large.code, 413);
  // None of it reached the loop task.
  EXPECT_EQ(this->dashboard->jobs, jobs);
  EXPECT_EQ(scan.offset(0), 0.0f);
  // A confirmation is not asked for, and not in the way either.
  Reply confirmed = this->post(OFFSET, this->confirmed(R"("slot":1,"offset":0.3)"));
  EXPECT_EQ(confirmed.code, 200) << confirmed.error();
  EXPECT_FLOAT_EQ(scan.offset(0), 0.3f);
}

// The offset belongs to the slot number: a forget of that slot and an assign into it leave it.
TEST_F(TemperatureSlots, AForgetOrAnAssignLeavesTheOffsetOnItsSlot) {
  TestScan &scan = this->boot({ROM_A, ROM_B});
  ASSERT_EQ(this->set_offset(1, "0.3").code, 200);
  Reply forget = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(forget.message(), "Slot 1 forgotten; applies after a reboot");
  Reply assign = this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x8a0122791699dd28")"));
  EXPECT_EQ(assign.code, 200) << assign.error();
  EXPECT_FLOAT_EQ(scan.offset(0), 0.3f);
  EXPECT_EQ(scan.offset(1), 0.0f);
  Reply list = this->get(SLOTS);
  JsonArray slots = list["slots"].as<JsonArray>();
  EXPECT_FLOAT_EQ(slots[0]["offset"].as<float>(), 0.3f);
  EXPECT_EQ(slots[1]["offset"].as<float>(), 0.0f);
}

// Forget All clears every offset at once, while the slots wait for the reboot as before.
TEST_F(TemperatureSlots, ForgetAllClearsTheOffsetsAtOnce) {
  this->bus.set_reading(ROM_A, 20.0f);
  TestScan &scan = this->boot({ROM_A, ROM_B});
  scan.poll();
  ASSERT_EQ(this->set_offset(1, "0.5").code, 200);
  ASSERT_EQ(this->set_offset(4, "-1").code, 200);
  ASSERT_FLOAT_EQ(scan.sensor(0)->state, 20.5f);
  Reply reply = this->post(FORGET, this->confirmed(R"("all":true)"));
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.message(), "Offsets cleared; every slot forgotten, applies after a reboot");
  EXPECT_TRUE(reply["reboot_required"].as<bool>());
  EXPECT_EQ(scan.offset(0), 0.0f);
  EXPECT_EQ(scan.offset(3), 0.0f);
  EXPECT_FLOAT_EQ(scan.sensor(0)->state, 20.0f);
  Reply list = this->get(SLOTS);
  JsonArray slots = list["slots"].as<JsonArray>();
  ASSERT_EQ(slots.size(), 2u);  // what ran at boot; the free slot 4 has nothing to show now
  for (JsonObject slot : slots)
    EXPECT_EQ(slot["offset"].as<float>(), 0.0f);
}

// Offsets alone are something to forget: no longer a 409, and nothing waits for a reboot.
TEST_F(TemperatureSlots, ForgetAllWithOnlyOffsetsLeftClearsThem) {
  TestScan &scan = this->boot({}, true);
  ASSERT_EQ(this->set_offset(2, "0.5").code, 200);
  // Forgetting that one slot would keep its offset, so it has nothing to forget.
  Reply list = this->get(SLOTS);
  ASSERT_EQ(list["slots"].as<JsonArray>().size(), 2u);
  EXPECT_FALSE(list["slots"][1]["can_forget"].as<bool>());
  EXPECT_EQ(this->post(FORGET, this->confirmed(R"("slot":2)")).code, 409);
  Reply reply = this->post(FORGET, this->confirmed(R"("all":true)"));
  EXPECT_EQ(reply.code, 200) << reply.error();
  EXPECT_EQ(reply.message(), "Offsets cleared; every slot forgotten");
  EXPECT_FALSE(reply["reboot_required"].as<bool>());
  EXPECT_EQ(scan.offset(1), 0.0f);
  EXPECT_FALSE(this->waits());
  EXPECT_EQ(this->get(SLOTS)["slots"].as<JsonArray>().size(), 1u);  // the listed slot alone
  // Cleared, so a second time there is nothing left.
  Reply again = this->post(FORGET, this->confirmed(R"("all":true)"));
  EXPECT_EQ(again.code, 409);
  EXPECT_EQ(again.error(), "Nothing to forget: every slot is free or listed in YAML");
}

// What the page's Forget All asks before it offers itself: a device or an offset to forget.
TEST_F(TemperatureSlots, CanForgetAllSaysWhetherForgetAllWouldChangeAnything) {
  this->boot({ROM_A});
  Reply devices = this->get(SLOTS);
  EXPECT_TRUE(devices["can_forget_all"].is<bool>());
  EXPECT_TRUE(devices["can_forget_all"].as<bool>());

  global_preferences->reset();
  this->boot({}, true);
  Reply nothing = this->get(SLOTS);
  EXPECT_FALSE(nothing["can_forget_all"].as<bool>());
  ASSERT_EQ(this->set_offset(3, "0.5").code, 200);
  Reply offsets = this->get(SLOTS);
  EXPECT_TRUE(offsets["can_forget_all"].as<bool>());
  // No row says it: forgetting slot 3 alone would keep its offset.
  for (JsonObject slot : offsets["slots"].as<JsonArray>())
    EXPECT_FALSE(slot["can_forget"].as<bool>()) << offsets.body;
}

// A listed slot's offset left in storage is dropped at boot, so it is not one to forget.
TEST_F(TemperatureSlots, AListedSlotsStoredOffsetIsNothingToForget) {
  const int16_t stored[] = {5, 0, 0, 0};
  ASSERT_TRUE(global_preferences->make_preference(sizeof(stored), fnv1_hash_extend(fnv1_hash("temps"), "offsets"))
                  .save(reinterpret_cast<const uint8_t *>(stored), sizeof(stored)));
  this->boot({}, true);
  Reply list = this->get(SLOTS);
  EXPECT_FALSE(list["can_forget_all"].as<bool>());
  EXPECT_TRUE(list["slots"][0]["offset"].isUnbound());
  Reply reply = this->post(FORGET, this->confirmed(R"("all":true)"));
  EXPECT_EQ(reply.code, 409);
  EXPECT_EQ(reply.error(), "Nothing to forget: every slot is free or listed in YAML");
}

// Until the reboot a swapped slot reads the device it booted with; the offset goes on that.
TEST_F(TemperatureSlots, AnOffsetSetWhileASwapWaitsGoesOnTheBootedDevicesReading) {
  this->bus.set_reading(ROM_A, 20.0f);
  this->bus.set_reading(ROM_B, 30.0f);
  TestScan &scan = this->boot({ROM_A, ROM_B});
  scan.poll();
  ASSERT_EQ(this->post(ASSIGN, this->confirmed(R"("slot":1,"address":"0x8a0122791699dd28")")).code, 200);
  Reply set = this->set_offset(1, "0.5");
  EXPECT_EQ(set.code, 200) << set.error();
  EXPECT_FLOAT_EQ(scan.sensor(0)->state, 20.5f);
  EXPECT_FLOAT_EQ(scan.sensor(1)->state, 30.0f);
  Reply list = this->get(SLOTS);
  EXPECT_TRUE(list["slots"][0]["pending"].as<bool>());
  EXPECT_EQ(list["slots"][0]["running_address"].as<std::string>(), "0xeb01227905460228");
  EXPECT_FLOAT_EQ(list["slots"][0]["offset"].as<float>(), 0.5f);
  EXPECT_TRUE(this->waits());
}

// A slot file that did not load waits for a person to fix it: no offset is written over it this
// boot, while a forget and an assign still are.
TEST_F(TemperatureSlots, AnOffsetWaitsForASlotFileThatLoads) {
  mkdir(".storage", 0755);
  char folder[] = ".storage/XXXXXX";
  ASSERT_NE(mkdtemp(folder), nullptr);
  const std::string dir = std::string(folder) + "/config";
  const std::string file = dir + "/dallas_scan_temps.json";
  ASSERT_EQ(mkdir(dir.c_str(), 0755), 0);
  {
    std::ofstream out(file);
    out << R"({"version":1,"records":[)";
  }
  static dir_storage::DirStorage storage;
  storage.set_base_path(folder);
  storage.setup();
  static config_json::ConfigJsonKeeper keeper;
  keeper.set_storage(&storage);
  keeper.setup();
  this->bus.set_devices({ROM_A});
  auto owned = std::make_unique<TestScan>();
  TestScan &scan = *owned;
  scan.set_one_wire_bus(&this->bus);
  scan.set_max_sensors(4);
  scan.set_slot_file(&keeper, "dallas_scan_temps");
  scan.setup();
  this->dashboard->set_temperature_slots(&scan);
  this->boots.push_back(std::move(owned));
  ASSERT_FALSE(scan.can_set_offset());

  for (const char *value : {"0.3", "0"}) {
    Reply offset = this->set_offset(1, value);
    EXPECT_EQ(offset.code, 503) << value;
    EXPECT_EQ(offset.error(), "The slot file did not load; fix it and reboot") << value;
  }
  EXPECT_EQ(scan.offset(0), 0.0f);
  Reply list = this->get(SLOTS);
  EXPECT_EQ(list.code, 200);
  EXPECT_TRUE(list["can_forget_all"].as<bool>());
  Reply forget = this->post(FORGET, this->confirmed(R"("slot":1)"));
  EXPECT_EQ(forget.code, 200) << forget.error();
  remove(file.c_str());
  rmdir(dir.c_str());
  rmdir(folder);
}

}  // namespace esphome::web_device_dashboard::testing
