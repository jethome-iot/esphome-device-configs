#pragma once

#include <cstdint>
#include <string_view>

namespace esphome {
namespace web_file_browser {

enum class RouteId : uint8_t { INFO, LIST, READ, DOWNLOAD, WRITE, UPLOAD, DELETE, MKDIR, RENAME, COPY };

struct Route {
  const char *name;
  RouteId id;
  /// Mutating routes answer POST only, read-only ones GET only. A GET that writes
  /// is one <img src> away from being fired by any page the browser opens.
  bool mutating;
};

// clang-format off
static const Route ROUTES[] = {
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

/// The route @p url names under @p prefix, or nullptr when it names none. The name
/// is the whole tail: "readme" is not a read and "list/" is not a list.
inline const Route *route_for(std::string_view url, std::string_view prefix) {
  if (!url.starts_with(prefix) || url.size() <= prefix.size() || url[prefix.size()] != '/') {
    return nullptr;
  }
  std::string_view tail = url.substr(prefix.size() + 1);
  for (const Route &route : ROUTES) {
    if (tail == route.name) {
      return &route;
    }
  }
  return nullptr;
}

}  // namespace web_file_browser
}  // namespace esphome
