#pragma once

#include <cstdint>
#include <ctime>
#include <string>
// posix_tz.h is all behind USE_TIME_TIMEZONE and does not pull the defines in itself.
#include "esphome/core/defines.h"
#include "esphome/components/time/posix_tz.h"
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"
#include "esphome/core/optional.h"
#include "esphome/core/preferences.h"

namespace esphome::timezone_settings {

// What a chosen zone may hold. The record is fixed-width, so the preference never changes size.
static const size_t ZONE_NAME_MAX = 63;
static const size_t POSIX_MAX = 63;

struct StoredSettings {
  uint8_t follow_homeassistant;
  // Empty posix: no zone was ever chosen.
  char name[ZONE_NAME_MAX + 1];
  char posix[POSIX_MAX + 1];
};

/// Which clock's sync callback fired last.
enum class ClockSource : uint8_t { NONE, HOMEASSISTANT, SNTP, RTC };
/// Where the zone in effect comes from.
enum class ZoneSource : uint8_t { HOMEASSISTANT, DEFAULT, MANUAL };

/// Why a change was refused; NONE when it is stored and in effect.
enum class SetError : uint8_t { NONE, NAME_TOO_LONG, BAD_NAME, POSIX_TOO_LONG, BAD_POSIX, NO_ZONE, NOT_STORED };

const char *clock_source_name(ClockSource source);
const char *zone_source_name(ZoneSource source);
/// The text of a refusal, for an API answer and the log.
const char *set_error_text(SetError error);

/// A zone picked by hand: the IANA name it was picked as, "" for none, and its POSIX string.
struct Zone {
  std::string name;
  std::string posix;
};

/// The device's timezone: Home Assistant's, kept across reboots in a restoring global, the one
/// compiled in, or one chosen at run time and kept in its own preference.
class TimezoneSettings : public Component {
 public:
  // Right below the globals (HARDWARE), so Home Assistant's stored zone is back before this
  // applies it, and ahead of everything that reads the local time while it sets up.
  float get_setup_priority() const override { return setup_priority::HARDWARE - 1.0f; }
  void setup() override;
  void dump_config() override;

  // Generated string literals: they outlive the component.
  void set_default_zone(const char *zone, const char *posix) {
    this->default_name_ = zone;
    this->default_posix_ = posix;
  }
  /// The restoring global's value; that global saves it.
  void set_homeassistant_zone(time::ParsedTimezone *zone) { this->homeassistant_zone_ = zone; }
  void set_preference_hash(uint32_t hash) { this->preference_hash_ = hash; }
  /// Reports @p source as the clock's name when its sync callback fires; the Home Assistant
  /// clock's also hands over the zone it may send.
  void add_clock(time::RealTimeClock *clock, ClockSource source);

  bool follow_homeassistant() const { return this->follow_homeassistant_; }
  ZoneSource zone_source() const;
  /// The zone this component puts in force; time::get_global_tz() may differ from it for one
  /// loop pass after Home Assistant sends a zone the device does not follow.
  const time::ParsedTimezone &zone_in_effect() const;
  bool has_chosen_zone() const { return !this->chosen_.posix.empty(); }
  const Zone &chosen_zone() const { return this->chosen_; }
  const char *default_zone() const { return this->default_name_; }
  ClockSource clock_source() const { return this->clock_source_; }

  /// Why @p zone could not be chosen, or NONE; @p parsed is filled when it can.
  static SetError check_zone(const Zone &zone, time::ParsedTimezone &parsed);

  /// Follows Home Assistant or the chosen zone; with @p zone, that becomes the chosen one,
  /// applied only when not following. Stored before applied: anything but NONE leaves the
  /// device as it was.
  SetError configure(bool follow_homeassistant, const Zone *zone = nullptr);
  SetError set_follow_homeassistant(bool follow) { return this->configure(follow); }
  SetError set_zone(const Zone &zone) { return this->configure(this->follow_homeassistant_, &zone); }

  /// What the API answers, read at one instant.
  struct Status {
    optional<time_t> time;  // UTC; none while the clock is not set
    int64_t utc_offset;     // seconds east of UTC at `time`; 64 bits, as -INT32_MIN is not 32
    bool dst;
  };
  Status status() const;
  /// The zone in effect as the panel shows it: the chosen zone's city, else its offset from UTC.
  std::string zone_label() const;

 protected:
  /// The clock; virtual so the tests can set the instant.
  virtual time_t now_() const { return ::time(nullptr); }
  // Puts the record in flash and flushes it; false when the flush reported a failure. Virtual so
  // the host tests can watch a store fail: the backend takes every write.
  virtual bool store_(const StoredSettings &stored);
  // Whether flash holds that record now, read back through the preference.
  bool flash_holds_(const StoredSettings &stored);
  bool homeassistant_zone_known_() const;
  void apply_();
  void on_homeassistant_sync_();
  void after_homeassistant_sync_();

  const char *default_name_{"UTC"};
  const char *default_posix_{"UTC0"};
  time::ParsedTimezone default_{};
  // RAM only until a global is wired in.
  time::ParsedTimezone own_homeassistant_zone_{};
  time::ParsedTimezone *homeassistant_zone_{&own_homeassistant_zone_};
  // Home Assistant sent its zone during this boot. A stored UTC reads as nothing stored, as it
  // always has, but one sent now is followed.
  bool homeassistant_zone_received_{false};
  bool follow_homeassistant_{true};
  Zone chosen_;
  time::ParsedTimezone chosen_parsed_{};
  ClockSource clock_source_{ClockSource::NONE};
  // The zone in force when Home Assistant's sync fired, to tell whether it sent one after it.
  time::ParsedTimezone before_sync_{};
  bool sync_pending_{false};
  ESPPreferenceObject pref_;
  uint32_t preference_hash_{0};
};

/// Field by field: the struct has padding that a copy need not carry.
bool same_zone(const time::ParsedTimezone &a, const time::ParsedTimezone &b);
/// "UTC", "UTC+3", "UTC+5:30", "UTC-3:30" for @p seconds east of UTC.
std::string utc_offset_text(int64_t seconds);
/// What the panel calls @p name: its last path segment, '_' read as a space.
std::string zone_city(const std::string &name);

}  // namespace esphome::timezone_settings
