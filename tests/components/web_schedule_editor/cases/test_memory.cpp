#include "common.h"

// A heap that runs short: a Save or an import is refused with 507 and changes nothing, a read
// is 503, a change that was made is answered short but never told to try again, and nothing
// aborts.
namespace esphome::web_schedule_editor::testing {

static const char *const NO_ROOM = "Not enough free memory on the device to save this schedule";
static const char *const LOW = "Device is low on memory; try again";

// Refuses every allocation once `refuse` says so: there the change is made and only its answer
// is left to build.
struct RefuseOnce : ArduinoJson::Allocator {
  std::function<bool()> refuse;
  void *allocate(size_t size) override { return this->refuse() ? nullptr : malloc(size); }
  void deallocate(void *ptr) override { free(ptr); }
  void *reallocate(void *ptr, size_t size) override { return this->refuse() ? nullptr : realloc(ptr, size); }
};

TEST_F(Editor, ABodyTheHeapCannotHoldIsRefused) {
  BudgetAllocator none(0);
  this->editor->allocator = &none;
  for (const char *route : {"save", "import"}) {
    Reply reply = this->post(route, with(porch(), R"("id":"porch")"));
    EXPECT_EQ(reply.code, 507) << route;
    EXPECT_EQ(reply.error(), NO_ROOM) << route;
  }
  this->editor->allocator = nullptr;
  EXPECT_TRUE(this->files().empty());
  EXPECT_TRUE(engine().schedules().empty());
  // The buffer is gone with the request: the next one is read whole.
  EXPECT_EQ(this->post("save", porch()).code, 200);
}

TEST_F(Editor, ABodyTheParserRunsOutOfHeapOnIsRefused) {
  const std::string body = porch();
  BudgetAllocator tight(body.size() + 64);
  this->editor->allocator = &tight;
  Reply reply = this->post("save", body);
  this->editor->allocator = nullptr;
  EXPECT_EQ(reply.code, 507);
  EXPECT_EQ(reply.error(), NO_ROOM);
  EXPECT_TRUE(this->files().empty());
}

// Under the reserve a Save or an import is refused before its body is parsed: the old file and
// the schedule in RAM stay as they were.
TEST_F(Editor, ASaveOrImportUnderTheReserveChangesNothing) {
  const std::string id = this->create(porch());
  const std::string before = this->file(id + ".json");
  engine().heap_free = schedules::HEAP_RESERVE_BYTES - 1;
  struct Case {
    const char *route;
    std::string body;
  };
  for (const Case &c :
       {Case{"save", porch("Garden", "relay_2")}, Case{"save", with(porch("Porch 2"), R"("id":"porch")")},
        Case{"import", with(porch("Kitchen", "relay_2"), R"("id":"kitchen")")},
        Case{"import", with(porch("Porch 3"), R"("id":"porch")")}}) {
    Reply reply = this->post(c.route, c.body);
    EXPECT_EQ(reply.code, 507) << c.route << " " << c.body;
    EXPECT_EQ(reply.error(), NO_ROOM) << c.route;
  }
  engine().heap_free = SIZE_MAX;
  EXPECT_EQ(this->files(), std::vector<std::string>{"porch.json"});
  EXPECT_EQ(this->file("porch.json"), before);
  ASSERT_EQ(engine().schedules().size(), 1u);
  EXPECT_EQ(engine().get("porch")->name, "Porch");
  EXPECT_EQ(engine().get("porch")->revision, 0u);
}

// Room for the reserve but not for the parse on top of it: refused before the parse begins.
// With what a schedule must leave behind, it goes through.
TEST_F(Editor, ASaveNeedsRoomForItsParseOnTopOfTheReserve) {
  const std::string body = porch();
  engine().heap_free = schedules::HEAP_RESERVE_BYTES + schedules::parse_room(body.size()) - 1;
  Reply reply = this->post("save", body);
  EXPECT_EQ(reply.code, 507);
  EXPECT_EQ(reply.error(), NO_ROOM);
  engine().heap_free = schedules::HEAP_KEEP_BYTES;
  EXPECT_EQ(this->post("save", body).code, 200);
}

// With PSRAM the parse's room is PSRAM's, on top of its reserve.
TEST_F(Editor, WithPsramASaveNeedsRoomForItsParseThere) {
  const std::string body = porch();
  engine().psram_free = schedules::PSRAM_RESERVE_BYTES + schedules::parse_room(body.size()) - 1;
  Reply reply = this->post("save", body);
  EXPECT_EQ(reply.code, 507);
  EXPECT_EQ(reply.error(), NO_ROOM);
  EXPECT_TRUE(this->files().empty());
  engine().psram_free = schedules::PSRAM_RESERVE_BYTES + schedules::parse_room(body.size());
  EXPECT_EQ(this->post("save", body).code, 200);
}

// Room for the parse, but not for what a new schedule must leave once held: the engine's own
// refusal, through the API. An edit that holds no more than the schedule it replaces still goes.
TEST_F(Editor, TheEnginesOwnRefusalIs507AndAnEditThatDoesNotGrowGoesThrough) {
  this->create(porch());
  const std::string body = porch("Garden", "relay_2");
  engine().heap_free = schedules::HEAP_KEEP_BYTES - 1;
  ASSERT_TRUE(engine().has_heap_room(schedules::parse_room(body.size()))) << "the editor lets it through";
  Reply refused = this->post("save", body);
  EXPECT_EQ(refused.code, 507);
  EXPECT_EQ(refused.error(), NO_ROOM);
  EXPECT_TRUE(LogCapture::instance().has("'garden' not written: it would leave"));
  EXPECT_EQ(this->files(), std::vector<std::string>{"porch.json"});
  Reply edited = this->post("save", with(porch("Porch 2"), R"("id":"porch")"));
  EXPECT_EQ(edited.code, 200) << edited.body;
  EXPECT_EQ(engine().get("porch")->name, "Porch 2");
}

// A body that grows many times over in its document, a long list of zeros here, stops at the
// room it was given: 507, never an abort. A body of the same size that does not grow goes.
TEST_F(Editor, ABodyThatExpandsInItsDocumentIsRefused) {
  const std::string schedule = porch();
  std::string zeros = R"({"pad":[)";
  while (zeros.size() + schedule.size() + 3 < schedules::MAX_FILE_BYTES)
    zeros += "0,";
  zeros += "0]," + schedule.substr(1);
  ASSERT_LE(zeros.size(), schedules::MAX_FILE_BYTES);
  Reply reply = this->post("save", zeros);
  EXPECT_EQ(reply.code, 507);
  EXPECT_EQ(reply.error(), NO_ROOM);
  EXPECT_TRUE(this->files().empty());
  EXPECT_TRUE(LogCapture::instance().has("Save refused"));

  std::string text = R"({"pad":")" + std::string(zeros.size() - schedule.size() - 10, '0') + "\"," + schedule.substr(1);
  EXPECT_EQ(this->post("save", text).code, 200);
}

// The densest a real body gets, a full week of switch points each at a time of its own, fits
// the room its size gives.
TEST_F(Editor, AFullWeekOfSwitchPointsFitsItsParse) {
  std::string week;
  for (int day = 0; day < 7; day++) {
    week += std::string(day == 0 ? "" : ",") + "\"" + schedules::DAY_NAMES[day] + "\":[";
    for (int point = 0; point < 12; point++) {
      char item[48];
      snprintf(item, sizeof(item), R"(%s{"at":"%02d:%02d","mode":"%s"})", point == 0 ? "" : ",", point * 2, day * 7 + 3,
               point % 2 == 0 ? "a" : "b");
      week += item;
    }
    week += "]";
  }
  const std::string body =
      R"({"name":"Dense","modes":[{"id":"a","name":"A","actions":[]},{"id":"b","name":"B","actions":[]}],"week":{)" +
      week + "}}";
  engine().psram_free = schedules::PSRAM_RESERVE_BYTES + schedules::parse_room(body.size());
  Reply reply = this->post("save", body);
  EXPECT_EQ(reply.code, 200) << reply.body;
}

TEST_F(Editor, ANameOf16KiBIsA400) {
  std::string name(16 * 1024, 'x');
  name.resize(schedules::MAX_FILE_BYTES - porch("").size());
  Reply reply = this->post("save", porch(name.c_str()));
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Name is longer than 48 characters");
  EXPECT_TRUE(this->files().empty());
}

// Once the engine made the change, an answer the heap cannot hold is no "try again": a retry
// would meet the change. It is 200 with what a client cannot do without, written off the heap.
TEST_F(Editor, AChangeThatWasMadeIsAnsweredShortNeverBusy) {
  RefuseOnce refusing;
  const std::string porch_file = this->folder() + "/porch.json";
  struct Case {
    const char *route;
    std::string body;
    std::function<bool()> made;
    const char *answer;
  };
  auto exists = [](const std::string &path) { return access(path.c_str(), F_OK) == 0; };
  auto revision = [](const char *id) {
    const schedules::ScheduleConfig *config = engine().get(id);
    return config == nullptr ? -1 : static_cast<int>(config->revision);
  };
  for (const Case &c : {
           Case{"save", porch(), [&] { return exists(porch_file); },
                R"({"success":true,"message":"Schedule created","id":"porch"})"},
           Case{"save", with(porch("Porch 2"), R"("id":"porch")"), [&] { return revision("porch") == 1; },
                R"({"success":true,"message":"Schedule updated","id":"porch"})"},
           Case{"import", with(porch("Porch 3"), R"("id":"porch")"), [&] { return revision("porch") == 2; },
                R"({"success":true,"message":"Schedule replaced","id":"porch"})"},
           Case{"import", with(porch("Kitchen", "relay_2"), R"("id":"kitchen")"),
                [&] { return exists(this->folder() + "/kitchen.json"); },
                R"({"success":true,"message":"Schedule created","id":"kitchen"})"},
           Case{"enable?id=porch&value=false", "", [&] { return !engine().get("porch")->enabled; },
                R"({"success":true,"message":"Schedule disabled","persisted":true})"},
           Case{"delete?id=porch", "", [&] { return !exists(porch_file); },
                R"({"success":true,"message":"Schedule deleted","persisted":true})"},
       }) {
    refusing.refuse = c.made;
    this->editor->allocator = &refusing;
    Reply reply = this->post(c.route, c.body);
    this->editor->allocator = nullptr;
    EXPECT_EQ(reply.code, 200) << c.route;
    EXPECT_EQ(reply.body, c.answer) << c.route;
  }
  EXPECT_TRUE(LogCapture::instance().has("An answer did not fit the heap"));
  EXPECT_EQ(this->files(), std::vector<std::string>{"kitchen.json"});
}

TEST_F(Editor, AReadTheHeapCannotHoldIsBusyNotACrash) {
  this->create(porch());
  BudgetAllocator none(0);
  this->editor->allocator = &none;
  for (const char *route : {"list", "get?id=porch", "targets"}) {
    Reply reply = this->get(route);
    EXPECT_EQ(reply.code, 503) << route;
    EXPECT_EQ(reply.error(), LOW) << route;
  }
  this->editor->allocator = nullptr;
  EXPECT_EQ(this->get("list").code, 200);
}

// Files the boot left on flash for lack of memory: the list counts them, a delete of one removes
// its file, and a schedule under a new id waits until they are gone.
TEST_F(Editor, FilesLeftOnFlashAreCountedDeletableAndHoldBackANewOne) {
  this->create(porch());
  this->create(porch("Garden", "relay_2"));
  engine().heap_free = schedules::BOOT_HEAP_FLOOR_BYTES - 1;
  this->reboot();
  engine().heap_free = SIZE_MAX;
  Reply list = this->get("list");
  EXPECT_EQ(list["count"].as<int>(), 0);
  EXPECT_EQ(list["not_loaded"].as<int>(), 2);

  Reply refused = this->post("import", with(porch("Kitchen", "relay_1"), R"("id":"kitchen")"));
  EXPECT_EQ(refused.code, 507);
  EXPECT_EQ(refused.error(),
            "2 saved schedules were not loaded for lack of memory; delete them, or make room and restart, before "
            "adding another");
  EXPECT_EQ(this->post("save", porch("Kitchen", "relay_1")).code, 507);

  Reply deleted = this->post("delete?id=porch");
  EXPECT_EQ(deleted.code, 200);
  EXPECT_EQ(deleted.message(), "Schedule deleted; it was not loaded for lack of memory");
  EXPECT_TRUE(deleted["persisted"].as<bool>());
  EXPECT_EQ(this->files(), std::vector<std::string>{"garden.json"});
  EXPECT_EQ(this->get("list")["not_loaded"].as<int>(), 1);
  EXPECT_EQ(this->post("delete?id=porch").code, 404);
  EXPECT_EQ(this->post("delete?id=garden").code, 200);
  EXPECT_EQ(this->get("list")["not_loaded"].as<int>(), 0);
  EXPECT_EQ(this->post("save", porch("Kitchen", "relay_1")).code, 200);
}

// An answer goes out a chunk at a time and still reads as one document.
TEST_F(Editor, ALongAnswerIsWholeAcrossItsChunks) {
  const std::string id = this->create(living_room_week());
  this->create(porch());
  this->create(porch("Garden", "relay_2"));
  Reply list = this->get("list");
  ASSERT_EQ(list.code, 200);
  EXPECT_GT(list.body.size(), 512u) << "more than one chunk";
  EXPECT_EQ(list["schedules"].size(), 3u);
  Reply doc = this->get("get?id=" + id);
  ASSERT_EQ(doc.code, 200);
  EXPECT_EQ(doc.body, this->file(id + ".json"));
}

}  // namespace esphome::web_schedule_editor::testing
