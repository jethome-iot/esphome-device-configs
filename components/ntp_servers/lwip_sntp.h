#pragma once

#include "esphome/core/defines.h"

#ifdef USE_ESP32
namespace esphome::ntp_servers {

/// Stops lwIP's SNTP client, clears every server slot and, given @p server, starts it again on
/// that one alone: one step in lwIP's context, so no reply is taken between the stop and the new
/// name. lwIP keeps @p server's pointer.
void restart_sntp(const char *server);

}  // namespace esphome::ntp_servers
#endif
