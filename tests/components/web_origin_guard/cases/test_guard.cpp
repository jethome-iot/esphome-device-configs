#include <gtest/gtest.h>
#include <cstdint>
#include <string>
#include <utility>
#include "esphome/components/web_origin_guard/web_origin_guard.h"

namespace esphome::web_origin_guard::testing {

static const char *const REFUSED = "Cross-origin request refused";

// The handler behind the guard: it answers what it is given and records every entry point it
// was reached on, so a case can say the guard stopped a request rather than only that the
// answer was a 403.
class Recorder : public AsyncWebHandler {
 public:
  // NOLINTNEXTLINE(readability-identifier-naming)
  bool canHandle(AsyncWebServerRequest *request) const override { return request->url().rfind("/mine", 0) == 0; }
  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleRequest(AsyncWebServerRequest *request) override {
    this->requests++;
    request->send(200, "text/plain", "served");
  }
  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override {
    this->bodies++;
    this->body.append(reinterpret_cast<const char *>(data), len);
  }
  // NOLINTNEXTLINE(readability-identifier-naming)
  void handleUpload(AsyncWebServerRequest *request, const std::string &filename, size_t index, uint8_t *data,
                    size_t len, bool final) override {
    this->uploads++;
    this->uploaded.append(reinterpret_cast<const char *>(data), len);
  }
  // NOLINTNEXTLINE(readability-identifier-naming)
  bool isRequestHandlerTrivial() const override { return this->trivial; }

  void reset() { *this = Recorder{}; }

  int requests{0};
  int bodies{0};
  int uploads{0};
  std::string body;
  std::string uploaded;
  bool trivial{false};
};

struct Reply {
  bool claimed{false};
  int code{0};
  std::string type;
  std::string body;
  int responses{0};
};

class Guard : public ::testing::Test {
 protected:
  void SetUp() override {
    this->base.init();
    this->base.add_handler(&this->guard);
  }

  // One request through the server, with the headers a browser would have sent.
  Reply call(http_method method, const std::string &target, const char *origin, const char *host,
             const std::string &body = "", const char *upload = nullptr) {
    AsyncWebServerRequest request(method, target, body, "text/plain");
    if (origin != nullptr)
      request.set_header("Origin", origin);
    if (host != nullptr)
      request.set_header("Host", host);
    if (upload != nullptr)
      request.set_upload("notes.txt", upload);
    Reply reply;
    reply.claimed = this->base.get_server()->dispatch(request);
    reply.code = request.response_code;
    reply.type = request.response_type;
    reply.body = request.response_body;
    reply.responses = request.responses;
    return reply;
  }

  // Nothing of the request reached the handler, on any of its three entry points.
  void expect_untouched() const {
    EXPECT_EQ(this->inner.requests, 0);
    EXPECT_EQ(this->inner.bodies, 0);
    EXPECT_EQ(this->inner.uploads, 0);
    EXPECT_EQ(this->inner.body, "");
    EXPECT_EQ(this->inner.uploaded, "");
  }

  Recorder inner;
  WebOriginGuard guard{&this->inner};
  web_server_base::WebServerBase base;
};

TEST_F(Guard, ServesARequestThatCarriesNoOrigin) {
  // curl, a script, a Home Assistant rest_command: no Origin, no browser, nothing to refuse.
  const Reply reply = this->call(HTTP_POST, "/mine/write", nullptr, "device.local", "hello");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(reply.body, "served");
  EXPECT_EQ(reply.responses, 1);
  EXPECT_EQ(this->inner.requests, 1);
  EXPECT_EQ(this->inner.body, "hello");
}

TEST_F(Guard, ServesTheDevicesOwnPage) {
  // What the page served from / sends: fetch sets Origin on every POST, same-origin included.
  // The device is reached by mDNS name, by IP and on whatever port it was given.
  for (const char *host : {"device.local", "192.168.1.50", "device.local:8080"}) {
    this->inner.reset();
    const Reply reply = this->call(HTTP_POST, "/mine/write", (std::string("http://") + host).c_str(), host, "hello");
    EXPECT_EQ(reply.code, 200) << host;
    EXPECT_EQ(this->inner.requests, 1) << host;
    EXPECT_EQ(this->inner.body, "hello") << host;
  }
}

TEST_F(Guard, RefusesAnotherSite) {
  const Reply reply = this->call(HTTP_POST, "/mine/write", "http://evil.example", "device.local", "hello");
  EXPECT_TRUE(reply.claimed);
  EXPECT_EQ(reply.code, 403);
  EXPECT_EQ(reply.type, "text/plain");
  EXPECT_EQ(reply.body, REFUSED);
  EXPECT_EQ(reply.responses, 1);
  this->expect_untouched();
}

TEST_F(Guard, RefusesAnotherPort) {
  // A different port is a different origin, both ways round.
  const std::pair<const char *, const char *> cases[] = {{"http://device.local:8080", "device.local"},
                                                         {"http://device.local", "device.local:8080"}};
  for (const auto &pair : cases) {
    this->inner.reset();
    const Reply reply = this->call(HTTP_POST, "/mine/write", pair.first, pair.second, "hello");
    EXPECT_EQ(reply.code, 403) << pair.first << " -> " << pair.second;
    EXPECT_EQ(reply.body, REFUSED) << pair.first << " -> " << pair.second;
    this->expect_untouched();
  }
}

TEST_F(Guard, RefusesAnOriginWithAnotherSchemeAndAnotherAuthority) {
  const Reply reply = this->call(HTTP_POST, "/mine/write", "https://evil.example", "device.local", "hello");
  EXPECT_EQ(reply.code, 403);
  EXPECT_EQ(reply.body, REFUSED);
  this->expect_untouched();
}

TEST_F(Guard, ServesAnOriginWhoseAuthorityIsTheHostWhateverTheScheme) {
  // The rule compares authorities, as web_server's own does: a device reached over TLS through
  // a proxy is still the same host, and an attacker cannot serve https on it to forge this.
  const Reply reply = this->call(HTTP_POST, "/mine/write", "https://device.local", "device.local", "hello");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(this->inner.requests, 1);
}

TEST_F(Guard, ReadsTheHeadersWhateverCaseTheyAreSpeltIn) {
  // HTTP header names are case-insensitive and a browser is free to send either spelling;
  // ESP-IDF's lookup ignores case, so the whole rule rests on this.
  AsyncWebServerRequest same(HTTP_POST, "/mine/write", "hello", "text/plain");
  same.set_header("origin", "http://device.local");
  same.set_header("host", "device.local");
  EXPECT_TRUE(this->base.get_server()->dispatch(same));
  EXPECT_EQ(same.response_code, 200);
  EXPECT_EQ(this->inner.requests, 1);

  this->inner.reset();
  AsyncWebServerRequest other(HTTP_POST, "/mine/write", "hello", "text/plain");
  other.set_header("ORIGIN", "http://evil.example");
  other.set_header("HOST", "device.local");
  this->base.get_server()->dispatch(other);
  EXPECT_EQ(other.response_code, 403);
  this->expect_untouched();
}

TEST_F(Guard, ComparesAnIpv6LiteralWholeBracketsAndAll) {
  const Reply same = this->call(HTTP_POST, "/mine/write", "http://[fe80::1]", "[fe80::1]", "hello");
  EXPECT_EQ(same.code, 200);
  EXPECT_EQ(this->inner.requests, 1);

  this->inner.reset();
  const Reply other = this->call(HTTP_POST, "/mine/write", "http://[fe80::2]", "[fe80::1]", "hello");
  EXPECT_EQ(other.code, 403);
  this->expect_untouched();
}

TEST_F(Guard, RefusesAnOriginThatNamesNoAuthority) {
  // What a sandboxed frame and a file:// page send.
  const Reply reply = this->call(HTTP_POST, "/mine/write", "null", "device.local", "hello");
  EXPECT_EQ(reply.code, 403);
  this->expect_untouched();
}

TEST_F(Guard, RefusesACrossOriginRequestWhenThereIsNoHostToCompare) {
  const Reply reply = this->call(HTTP_POST, "/mine/write", "http://device.local", nullptr, "hello");
  EXPECT_EQ(reply.code, 403);
  this->expect_untouched();
}

TEST_F(Guard, RefusesACrossOriginUploadBeforeTheHandlerOpensAnything) {
  // The upload lands before handleRequest and a handler writes it as it arrives, so the guard
  // has to stop it there and not only answer afterwards.
  const Reply reply = this->call(HTTP_POST, "/mine/upload", "http://evil.example", "device.local", "", "file bytes");
  EXPECT_EQ(reply.code, 403);
  EXPECT_EQ(reply.responses, 1);
  this->expect_untouched();
}

TEST_F(Guard, ServesAnUploadFromTheDevicesOwnPage) {
  const Reply reply = this->call(HTTP_POST, "/mine/upload", "http://device.local", "device.local", "", "file bytes");
  EXPECT_EQ(reply.code, 200);
  EXPECT_EQ(this->inner.uploads, 1);
  EXPECT_EQ(this->inner.uploaded, "file bytes");
}

TEST_F(Guard, RefusesACrossOriginGetToo) {
  // Every request the handler claims, not only the mutating ones: that is what web_server does,
  // and a GET a browser navigates to carries no Origin, so the device's own page is unaffected.
  const Reply reply = this->call(HTTP_GET, "/mine/list", "http://evil.example", "device.local");
  EXPECT_EQ(reply.code, 403);
  this->expect_untouched();
}

TEST_F(Guard, ClaimsExactlyWhatTheHandlerClaims) {
  // A request the handler does not want stays unclaimed, cross-origin or not, so it falls
  // through to whatever handler comes next instead of being answered with a 403.
  const char *const origins[] = {nullptr, "http://evil.example"};
  for (const char *origin : origins) {
    const Reply reply = this->call(HTTP_GET, "/other/list", origin, "device.local");
    EXPECT_FALSE(reply.claimed) << (origin == nullptr ? "no origin" : origin);
    EXPECT_EQ(reply.responses, 0) << (origin == nullptr ? "no origin" : origin);
  }
  this->expect_untouched();
}

TEST_F(Guard, CarriesTheHandlersOwnAnswerAboutRawBodies) {
  // The Arduino server hands a raw body only to a handler that says it wants one; the guard is
  // what the server sees, so it has to answer for the handler behind it.
  EXPECT_FALSE(this->guard.isRequestHandlerTrivial());
  this->inner.trivial = true;
  EXPECT_TRUE(this->guard.isRequestHandlerTrivial());
}

TEST_F(Guard, JudgesARequestWithoutWrappingIt) {
  AsyncWebServerRequest same(HTTP_POST, "/mine/write");
  same.set_header("Origin", "http://device.local");
  same.set_header("Host", "device.local");
  EXPECT_TRUE(allowed(&same));

  AsyncWebServerRequest other(HTTP_POST, "/mine/write");
  other.set_header("Origin", "http://evil.example");
  other.set_header("Host", "device.local");
  EXPECT_FALSE(allowed(&other));

  AsyncWebServerRequest bare(HTTP_POST, "/mine/write");
  EXPECT_TRUE(allowed(&bare));
}

// --- the catch-all ---

// What the refuser stands in front of: a handler it does not wrap and cannot reach into --
// upstream's REST routes, its OTA handler at /update, whatever a firmware adds next.
class Refuser : public ::testing::Test {
 protected:
  void SetUp() override {
    this->refuser.setup();
    // After it, as every handler-registering component is: the refuser sets up at
    // setup_priority::WIFI and they are all below it.
    this->base.add_handler(&this->other);
  }

  Reply call(http_method method, const std::string &target, const char *origin, const std::string &body = "",
             const char *upload = nullptr) {
    AsyncWebServerRequest request(method, target, body, "multipart/form-data");
    request.set_header("Host", "device.local");
    if (origin != nullptr)
      request.set_header("Origin", origin);
    if (upload != nullptr)
      request.set_upload("firmware.bin", upload);
    Reply reply;
    reply.claimed = this->base.get_server()->dispatch(request);
    reply.code = request.response_code;
    reply.type = request.response_type;
    reply.body = request.response_body;
    reply.responses = request.responses;
    return reply;
  }

  web_server_base::WebServerBase base;
  CrossOriginRefuser refuser{&this->base};
  // Claims a path no wrapper of ours covers, the way upstream's OTA handler claims /update.
  Recorder other;
};

TEST_F(Refuser, RefusesACrossSiteUploadToAHandlerItDoesNotWrap) {
  const Reply reply = this->call(HTTP_POST, "/mine/update", "http://evil.example", "", "firmware bytes");
  EXPECT_TRUE(reply.claimed);
  EXPECT_EQ(reply.code, 403);
  EXPECT_EQ(reply.type, "text/plain");
  EXPECT_EQ(reply.body, REFUSED);
  EXPECT_EQ(reply.responses, 1);
  EXPECT_EQ(this->other.requests, 0);
  EXPECT_EQ(this->other.uploads, 0);
  EXPECT_EQ(this->other.uploaded, "");
}

TEST_F(Refuser, DrainsACrossSiteBodyIntoNothing) {
  const Reply reply = this->call(HTTP_POST, "/mine/write", "http://evil.example", "a body nobody asked for");
  EXPECT_EQ(reply.code, 403);
  EXPECT_EQ(reply.responses, 1);
  EXPECT_EQ(this->other.bodies, 0);
  EXPECT_EQ(this->other.body, "");
}

TEST_F(Refuser, ClaimsNothingItIsNotRefusing) {
  // An allowed request must fall through to the handler that would have served it, so the
  // refuser can sit in front of everything without answering anything.
  for (const char *origin : {"http://device.local", "https://device.local"}) {
    this->other.reset();
    const Reply reply = this->call(HTTP_POST, "/mine/write", origin, "hello");
    EXPECT_TRUE(reply.claimed) << origin;
    EXPECT_EQ(reply.code, 200) << origin;
    EXPECT_EQ(this->other.requests, 1) << origin;
    EXPECT_EQ(this->other.body, "hello") << origin;
  }
  this->other.reset();
  const Reply bare = this->call(HTTP_POST, "/mine/write", nullptr, "hello");
  EXPECT_EQ(bare.code, 200);
  EXPECT_EQ(this->other.requests, 1);
}

TEST_F(Refuser, LeavesAnAllowedRequestForNobodyUnclaimed) {
  // Same-origin, and no handler wants it: the refuser must not turn a 404 into a 403.
  const Reply reply = this->call(HTTP_POST, "/nobody/here", "http://device.local", "hello");
  EXPECT_FALSE(reply.claimed);
  EXPECT_EQ(reply.responses, 0);
}

TEST_F(Refuser, AsksForRawBodiesSoAClaimedOneIsNotParsedAsForm) {
  EXPECT_FALSE(this->refuser.isRequestHandlerTrivial());
}

TEST_F(Refuser, SetsUpAheadOfEveryHandlerItStandsInFrontOf) {
  // It only covers what it is registered before, and the server asks its handlers in
  // registration order, which is setup order. web_server and our three are WIFI - 1.0 and
  // WIFI - 0.5; the web_server OTA platform, which serves /update, is AFTER_WIFI.
  EXPECT_GT(this->refuser.get_setup_priority(), setup_priority::WIFI - 0.5f);
  EXPECT_GT(this->refuser.get_setup_priority(), setup_priority::AFTER_WIFI);
}

}  // namespace esphome::web_origin_guard::testing
