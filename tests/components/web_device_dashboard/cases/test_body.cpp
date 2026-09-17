#include "common.h"

namespace esphome::web_device_dashboard::testing {

// MAX_BODY_BYTES, which lives in the component's translation unit.
static constexpr size_t CAP = 4096;

// A JSON update padded out to exactly @p size bytes.
static std::string padded(size_t size) {
  std::string body = R"json({"type":"switch","source_name":"relay_1","settings":{"inverted":true},"pad":""})json";
  body.insert(body.size() - 2, std::string(size - body.size(), 'x'));
  return body;
}

TEST_F(Dashboard, AssemblesABodyThatArrivesInChunks) {
  const std::string body = UPDATE_RELAY_1;
  this->feed(body, 7);
  EXPECT_EQ(this->dashboard->body(), body);
  EXPECT_EQ(this->dashboard->body_total(), body.size());
  EXPECT_EQ(this->dashboard->body_received(), body.size());
  EXPECT_FALSE(this->dashboard->body_too_large());
}

TEST_F(Dashboard, AChunkedPostIsHandledAsOneBody) {
  Reply reply = this->post("/api/device/entity-settings", UPDATE_RELAY_1, 7);
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(reply.success());
  EXPECT_EQ(store().sw.report(), "relay_1=1");
}

TEST_F(Dashboard, ABodyExactlyAtTheCapIsAccepted) {
  const std::string body = padded(CAP);
  ASSERT_EQ(body.size(), CAP);
  Reply reply = this->post("/api/device/entity-settings", body);
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(store().sw.report(), "relay_1=1");
}

TEST_F(Dashboard, ABodyOverTheCapIsRefused) {
  Reply reply = this->post("/api/device/entity-settings", padded(CAP + 1));
  EXPECT_EQ(reply.code, 413);
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "Request body over 4 KiB");
  EXPECT_EQ(store().sw.report(), "");
}

TEST_F(Dashboard, ABodyOverTheCapCostsNoHeap) {
  const std::string body = padded(CAP + 1);
  this->feed(body, 512);
  EXPECT_TRUE(this->dashboard->body_too_large());
  EXPECT_TRUE(this->dashboard->body().empty());
  // Nothing was appended, so the buffer never grew past what an empty string carries.
  EXPECT_EQ(this->dashboard->body_capacity(), std::string().capacity());
  // Counted anyway: what is still on the wire has to be recognised as this request's.
  EXPECT_EQ(this->dashboard->body_received(), body.size());
}

TEST_F(Dashboard, TheBufferIsReleasedOnceTheRequestIsAnswered) {
  Reply reply = this->post("/api/device/entity-settings", padded(CAP));
  ASSERT_EQ(reply.code, 200);
  EXPECT_TRUE(this->dashboard->body().empty());
  EXPECT_EQ(this->dashboard->body_capacity(), std::string().capacity());
  EXPECT_EQ(this->dashboard->body_total(), 0u);
  EXPECT_EQ(this->dashboard->body_received(), 0u);
  EXPECT_FALSE(this->dashboard->body_too_large());
}

TEST_F(Dashboard, ALeftoverBodyIsNotServedToTheNextRequest) {
  // A request that never reached handleRequest, its body half delivered.
  const std::string body = UPDATE_RELAY_1;
  this->feed(body, 8, 16);
  ASSERT_EQ(this->dashboard->body_received(), 16u);
  ASSERT_FALSE(this->dashboard->body().empty());

  Reply reply = this->post("/api/device/entity-settings");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Invalid JSON");
  EXPECT_EQ(store().sw.report(), "");
  EXPECT_TRUE(this->dashboard->body().empty());
}

TEST_F(Dashboard, ALeftoverBodyOfTheSameLengthIsNotServedEither) {
  const std::string body = UPDATE_RELAY_1;
  this->feed(body, 8, 16);
  ASSERT_EQ(this->dashboard->body_total(), body.size());

  // A form body is parsed into parameters and never reaches handleBody, so the counters
  // entering handleRequest are the leftover's while the Content-Length matches it.
  Reply reply = this->call(HTTP_POST, "/api/device/entity-settings", std::string(body.size(), 'a'), 512,
                           "application/x-www-form-urlencoded");
  EXPECT_EQ(reply.code, 400);
  EXPECT_EQ(reply.error(), "Invalid JSON");
  EXPECT_EQ(store().sw.report(), "");
}

TEST_F(Dashboard, AnEmptyPostBodyIsRefusedAsInvalidJson) {
  Reply reply = this->post("/api/device/entity-settings");
  EXPECT_EQ(reply.code, 400);
  EXPECT_FALSE(reply.success());
  EXPECT_EQ(reply.error(), "Invalid JSON");
}

TEST_F(Dashboard, ABodyIsIgnoredByTheReadRoutes) {
  // handleBody runs for every claimed request; a GET that carries one must not be disturbed.
  Reply reply = this->call(HTTP_GET, "/api/device/info", UPDATE_RELAY_1, 7);
  EXPECT_EQ(reply.code, 200);
  EXPECT_TRUE(this->dashboard->body().empty());
}

}  // namespace esphome::web_device_dashboard::testing
