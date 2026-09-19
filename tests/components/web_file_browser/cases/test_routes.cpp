#include <gtest/gtest.h>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include "esphome/components/web_file_browser/routes.h"

namespace esphome::web_file_browser::testing {

static constexpr std::string_view PREFIX = "/files";

// What the table has to say for the client and for the CSRF rule: every name its own id, and
// POST-only exactly where the request changes the filesystem.
struct Expected {
  const char *name;
  RouteId id;
  bool mutating;
};

// clang-format off
static const Expected EXPECTED[] = {
    {"info", RouteId::INFO, false},
    {"list", RouteId::LIST, false},
    {"read", RouteId::READ, false},
    {"download", RouteId::DOWNLOAD, false},
    {"write", RouteId::WRITE, true},
    {"upload", RouteId::UPLOAD, true},
    {"delete", RouteId::DELETE, true},
    {"mkdir", RouteId::MKDIR, true},
    {"rename", RouteId::RENAME, true},
    {"copy", RouteId::COPY, true},
};
// clang-format on

TEST(RouteFor, ResolvesEveryRouteToItsOwnId) {
  // A route added to the table without a line here would go untested.
  ASSERT_EQ(std::size(ROUTES), std::size(EXPECTED));
  std::set<RouteId> seen;
  for (const Expected &want : EXPECTED) {
    const Route *route = route_for(std::string(PREFIX) + "/" + want.name, PREFIX);
    ASSERT_NE(route, nullptr) << want.name;
    EXPECT_STREQ(route->name, want.name);
    EXPECT_EQ(route->id, want.id) << want.name;
    // The flag decides GET or POST, and a GET that writes is one <img src> away from being
    // fired by any page the browser opens.
    EXPECT_EQ(route->mutating, want.mutating) << want.name;
    EXPECT_TRUE(seen.insert(want.id).second) << want.name << " shares an id";
  }
}

TEST(RouteFor, MatchesTheWholeTail) {
  // clang-format off
  static const char *const NOT_A_ROUTE[] = {
      "",                  // nothing to match
      "/",
      "/other/list",       // another handler's prefix
      "/FILES/list",       // the prefix matches as it is written
      "//files/list",
      "/files",            // the prefix alone
      "/fileslist",        // no separator
      "/files/",           // the empty tail
      "/files/readme",     // "readme" is not a read
      "/files/LIST",       // and names match as they are written too
      "/files/list/",      // a trailing slash belongs to the tail
      "/files/list/sub",   // and so does a further segment
      "/files/list/list",
      "/files/list?p=/",   // url_to() hands over the path, query stripped
  };
  // clang-format on
  for (const char *url : NOT_A_ROUTE) {
    EXPECT_EQ(route_for(url, PREFIX), nullptr) << '"' << url << '"';
  }
}

TEST(RouteFor, MatchesUnderAnyPrefix) {
  const Route *list = route_for("/fs/list", "/fs");
  ASSERT_NE(list, nullptr);
  EXPECT_EQ(list->id, RouteId::LIST);
  const Route *copy = route_for("/api/v1/files/copy", "/api/v1/files");
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->id, RouteId::COPY);
  EXPECT_EQ(route_for("/fs/list", "/files"), nullptr);
}

}  // namespace esphome::web_file_browser::testing
