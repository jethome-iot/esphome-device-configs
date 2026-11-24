#include "web_origin_guard.h"

namespace esphome::web_origin_guard {

static char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

static bool equal_ignoring_case(std::string_view a, std::string_view b) {
  if (a.size() != b.size())
    return false;
  for (size_t i = 0; i < a.size(); i++) {
    if (lower(a[i]) != lower(b[i]))
      return false;
  }
  return true;
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

// One to five digits: what a Host header puts after the last ':'.
static bool is_port(std::string_view s) {
  if (s.empty() || s.size() > 5)
    return false;
  for (char c : s) {
    if (!is_digit(c))
      return false;
  }
  return true;
}

// Four dotted decimal parts of at most 255 each.
static bool is_ipv4(std::string_view s) {
  size_t parts = 0;
  size_t i = 0;
  while (true) {
    const size_t start = i;
    unsigned value = 0;
    while (i < s.size() && is_digit(s[i]) && i - start < 3) {
      value = value * 10 + static_cast<unsigned>(s[i] - '0');
      i++;
    }
    if (i == start || value > 255 || (i < s.size() && is_digit(s[i])))
      return false;
    parts++;
    if (i == s.size())
      return parts == 4;
    if (s[i] != '.' || parts == 4)
      return false;
    i++;
  }
}

// What a browser puts between the brackets of an IPv6 Host: hex digits and colons, and an
// embedded IPv4 tail's dots. A zone never goes into a Host (RFC 6874).
static bool is_ipv6_body(std::string_view s) {
  if (s.empty())
    return false;
  for (char c : s) {
    const bool hex = is_digit(c) || (lower(c) >= 'a' && lower(c) <= 'f');
    if (!hex && c != ':' && c != '.')
      return false;
  }
  return s.find(':') != std::string_view::npos;
}

bool host_names_device(std::string_view host, std::string_view name) {
  if (!host.empty() && host.front() == '[') {
    const size_t close = host.find(']');
    if (close == std::string_view::npos || !is_ipv6_body(host.substr(1, close - 1)))
      return false;
    const std::string_view rest = host.substr(close + 1);
    return rest.empty() || (rest.front() == ':' && is_port(rest.substr(1)));
  }
  if (const size_t colon = host.rfind(':'); colon != std::string_view::npos) {
    if (!is_port(host.substr(colon + 1)))
      return false;
    host = host.substr(0, colon);
  }
  // An absolute name ends in a dot.
  if (!host.empty() && host.back() == '.')
    host.remove_suffix(1);
  if (host.empty())
    return false;
  if (is_ipv4(host))
    return true;
  return !name.empty() && equal_ignoring_case(host.substr(0, host.find('.')), name);
}

}  // namespace esphome::web_origin_guard
