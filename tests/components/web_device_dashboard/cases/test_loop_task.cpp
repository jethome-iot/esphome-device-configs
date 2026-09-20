#include "common.h"

// The entity records belong to the loop task: a settings row on the display appends to the
// same vector these routes walk. A request arrives on the HTTP server's own task, so the read
// goes over together with the serialization -- a record read here could be one the menu had
// just reallocated away.
namespace esphome::web_device_dashboard::testing {

TEST_F(Dashboard, BothEntitySettingsRoutesGoOverToTheLoopTask) {
  this->dashboard->jobs = 0;
  ASSERT_EQ(this->post("/api/device/entity-settings", UPDATE_RELAY_1).code, 200);
  EXPECT_EQ(this->dashboard->jobs, 1);

  this->dashboard->jobs = 0;
  ASSERT_EQ(this->get("/api/device/entity-settings?type=switch").code, 200);
  EXPECT_EQ(this->dashboard->jobs, 1) << "the records were serialized on the server task";

  this->dashboard->jobs = 0;
  ASSERT_EQ(this->get("/api/device/entity-settings?type=switch&source_name=relay_1").code, 200);
  EXPECT_EQ(this->dashboard->jobs, 1) << "the record was serialized on the server task";
}

// The routes that answer from what the build already fixed: App's entity lists and the form
// fields a settings type declares. Holding the loop task up for them would be a cost for no
// reason -- the page polls them.
TEST_F(Dashboard, TheRoutesThatTouchNoRecordStayOnTheServerTask) {
  for (const char *url : {"/api/device/entities", "/api/device/entity-settings-meta", "/api/device/status"}) {
    this->dashboard->jobs = 0;
    EXPECT_EQ(this->get(url).code, 200) << url;
    EXPECT_EQ(this->dashboard->jobs, 0) << url << " went over to the loop task for nothing";
  }
}

// 503, not the 500 an unknown status turns into on ESP-IDF: nothing was read or written.
TEST_F(Dashboard, ALoopThatNeverTakesTheJobAnswersBusyAndChangesNothing) {
  ASSERT_EQ(this->post("/api/device/entity-settings", UPDATE_RELAY_1).code, 200);
  Dashboard::loop();

  this->dashboard->loop_busy = true;
  Reply read = this->get("/api/device/entity-settings?type=switch");
  EXPECT_EQ(read.code, 503);
  EXPECT_EQ(read.error(), "Device busy");
  Reply write = this->post("/api/device/entity-settings", DELETE_RELAY_1);
  EXPECT_EQ(write.code, 503);
  EXPECT_EQ(write.error(), "Device busy");
  this->dashboard->loop_busy = false;

  // Refused, not half done: the record the delete would have taken is still there.
  EXPECT_EQ(store().sw.report(), "relay_1=1");
  EXPECT_EQ(this->get("/api/device/entity-settings?type=switch")["records"].size(), 1u);
}

// A 404 raised inside the job is the job's answer, not the loop failing to run it.
TEST_F(Dashboard, AnUnknownTypeFoundOnTheLoopTaskKeepsItsOwnStatus) {
  Reply reply = this->get("/api/device/entity-settings?type=light");
  EXPECT_EQ(reply.code, 404);
  EXPECT_EQ(reply.error(), "Settings type not found");
}

}  // namespace esphome::web_device_dashboard::testing
