#include "common.h"

// A heap that runs short: a Save or an import is refused with 507 and changes nothing, a read
// is 503, and nothing aborts.
namespace esphome::web_schedule_editor::testing {

static const char *const NO_ROOM = "Not enough free memory on the device to save this schedule";
static const char *const LOW = "Device is low on memory; try again";

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

// The reserve is checked with the body parsed, before the engine sees the schedule: the old file
// and the schedule in RAM stay as they were.
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
  engine().heap_free = schedules::HEAP_RESERVE_BYTES + schedules::PARSE_HEAP_FACTOR * body.size() - 1;
  Reply reply = this->post("save", body);
  EXPECT_EQ(reply.code, 507);
  EXPECT_EQ(reply.error(), NO_ROOM);
  engine().heap_free = schedules::HEAP_KEEP_BYTES;
  EXPECT_EQ(this->post("save", body).code, 200);
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
