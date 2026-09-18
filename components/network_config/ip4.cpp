#include "ip4.h"
#include <cstdio>

namespace esphome::network_config {

bool parse_ip4(const std::string &text, uint32_t &address) {
  uint32_t value = 0;
  size_t pos = 0;
  for (uint8_t field = 0; field < 4; field++) {
    if (field != 0) {
      if (pos >= text.size() || text[pos] != '.')
        return false;
      pos++;
    }
    uint32_t octet = 0;
    size_t digits = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
      // A leading zero is octal to inet_aton, so "010.0.0.1" would not mean what it reads as.
      if (digits == 1 && octet == 0)
        return false;
      octet = octet * 10 + static_cast<uint32_t>(text[pos] - '0');
      pos++;
      digits++;
      if (digits > 3 || octet > 255)
        return false;
    }
    if (digits == 0)
      return false;
    value = (value << 8) | octet;
  }
  if (pos != text.size())
    return false;
  address = value;
  return true;
}

std::string format_ip4(uint32_t address) {
  char buffer[16];
  snprintf(buffer, sizeof(buffer), "%u.%u.%u.%u", static_cast<unsigned>((address >> 24) & 0xFF),
           static_cast<unsigned>((address >> 16) & 0xFF), static_cast<unsigned>((address >> 8) & 0xFF),
           static_cast<unsigned>(address & 0xFF));
  return buffer;
}

}  // namespace esphome::network_config
