#pragma once

#include <string>
// posix_tz.h is all behind USE_TIME_TIMEZONE and does not pull the defines in itself.
#include "esphome/core/defines.h"
#include "esphome/components/time/posix_tz.h"

namespace esphome::timezone_settings {

/// @p text as aioesphomeapi.posix_tz.parse_posix_tz reads it, into @p out; false where that
/// raises, @p out then untouched. ESPHome parses only at build time, so a zone chosen at run
/// time needs this. Stricter in two places: a byte outside printable ASCII, which Python would
/// take for a letter or a digit or skip, and a number the struct cannot hold.
bool parse_posix_tz(const std::string &text, time::ParsedTimezone &out);

}  // namespace esphome::timezone_settings
