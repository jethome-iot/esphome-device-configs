#pragma once

#include <cstdint>
#include <string>

namespace esphome::network_config {

// Strict dotted quad: four decimal fields of one to three digits, no leading zero unless the
// field is "0", every value at most 255 and nothing around them. It is the only gate there is:
// network::IPAddress cannot report a bad string (network/ip_address.h:118-121, :203-204) and the
// ipaddr_aton() behind it also takes "10.1", "0x0a000001", "010.0.0.1" and IPv6.
bool parse_ip4(const std::string &text, uint32_t &address);

inline bool is_ip4(const std::string &text) {
  uint32_t address = 0;
  return parse_ip4(text, address);
}

// Dotted quad of a host-order address, the form parse_ip4() accepts back.
std::string format_ip4(uint32_t address);

}  // namespace esphome::network_config
