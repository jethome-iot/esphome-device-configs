#include "timezone_settings.h"
#include <cstring>
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/time.h"
#include "posix_tz_parse.h"

namespace esphome::timezone_settings {

static const char *const TAG = "timezone_settings";
static const char *const HOMEASSISTANT_ZONE_READ = "homeassistant-zone";

const char *clock_source_name(ClockSource source) {
  switch (source) {
    case ClockSource::HOMEASSISTANT:
      return "homeassistant";
    case ClockSource::SNTP:
      return "sntp";
    case ClockSource::RTC:
      return "rtc";
    case ClockSource::NONE:
    default:
      return nullptr;
  }
}

const char *zone_source_name(ZoneSource source) {
  switch (source) {
    case ZoneSource::HOMEASSISTANT:
      return "homeassistant";
    case ZoneSource::MANUAL:
      return "manual";
    case ZoneSource::DEFAULT:
    default:
      return "default";
  }
}

const char *set_error_text(SetError error) {
  switch (error) {
    case SetError::NAME_TOO_LONG:
      return "'zone' is over 63 bytes";
    case SetError::BAD_NAME:
      return "'zone' cannot contain control characters";
    case SetError::POSIX_TOO_LONG:
      return "'posix' is over 63 bytes";
    case SetError::BAD_POSIX:
      return "'posix' is not a POSIX TZ string";
    case SetError::NO_ZONE:
      return "No zone was chosen yet: send 'posix' with it";
    case SetError::NOT_STORED:
      return "The time settings were not stored";
    case SetError::NONE:
    default:
      return "";
  }
}

static bool same_rule(const time::DSTRule &a, const time::DSTRule &b) {
  return a.time_seconds == b.time_seconds && a.day == b.day && a.type == b.type && a.month == b.month &&
         a.week == b.week && a.day_of_week == b.day_of_week;
}

bool same_zone(const time::ParsedTimezone &a, const time::ParsedTimezone &b) {
  return a.std_offset_seconds == b.std_offset_seconds && a.dst_offset_seconds == b.dst_offset_seconds &&
         same_rule(a.dst_start, b.dst_start) && same_rule(a.dst_end, b.dst_end);
}

std::string utc_offset_text(int64_t seconds) {
  if (seconds == 0)
    return "UTC";
  const char sign = seconds < 0 ? '-' : '+';
  const uint64_t magnitude = seconds < 0 ? -static_cast<uint64_t>(seconds) : static_cast<uint64_t>(seconds);
  const auto hours = static_cast<unsigned long long>(magnitude / 3600);
  const auto minutes = static_cast<unsigned>(magnitude % 3600 / 60);
  const auto secs = static_cast<unsigned>(magnitude % 60);
  if (secs != 0)
    return str_sprintf("UTC%c%llu:%02u:%02u", sign, hours, minutes, secs);
  if (minutes != 0)
    return str_sprintf("UTC%c%llu:%02u", sign, hours, minutes);
  return str_sprintf("UTC%c%llu", sign, hours);
}

std::string zone_city(const std::string &name) {
  const size_t slash = name.rfind('/');
  std::string city = slash == std::string::npos ? name : name.substr(slash + 1);
  for (char &c : city) {
    if (c == '_')
      c = ' ';
  }
  return city;
}

void TimezoneSettings::add_clock(time::RealTimeClock *clock, ClockSource source) {
  if (source == ClockSource::HOMEASSISTANT)
    this->homeassistant_clock_ = clock;
  clock->add_on_time_sync_callback([this, source]() {
    // The RTC is re-read every few minutes; it is the source only until the network has set the clock.
    if (source != ClockSource::RTC || this->clock_source_ == ClockSource::NONE)
      this->clock_source_ = source;
    if (source == ClockSource::HOMEASSISTANT)
      this->on_homeassistant_sync_();
  });
}

void TimezoneSettings::setup() {
  if (!parse_posix_tz(this->default_posix_, this->default_)) {
    // Validated at build time against the same rules, so only a broken build gets here.
    ESP_LOGE(TAG, "The compiled zone '%s' does not parse; running on UTC", this->default_posix_);
    this->default_ = time::ParsedTimezone{};
  }
  this->pref_ = global_preferences->make_preference<StoredSettings>(this->preference_hash_);
  StoredSettings stored{};
  // Nothing stored is a device that has only ever followed Home Assistant.
  if (this->pref_.load(&stored)) {
    stored.name[ZONE_NAME_MAX] = '\0';
    stored.posix[POSIX_MAX] = '\0';
    this->follow_homeassistant_ = stored.follow_homeassistant != 0;
    const Zone zone{stored.name, stored.posix};
    time::ParsedTimezone parsed;
    if (!zone.posix.empty()) {
      if (check_zone(zone, parsed) == SetError::NONE) {
        this->chosen_ = zone;
        this->chosen_parsed_ = parsed;
      } else {
        ESP_LOGE(TAG, "The stored zone '%s' does not parse; dropped", stored.posix);
      }
    }
    // configure() never stores that without a zone; a record that says it anyway follows.
    if (!this->follow_homeassistant_ && !this->has_chosen_zone())
      this->follow_homeassistant_ = true;
  }
  this->apply_();
}

void TimezoneSettings::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Timezone settings:\n"
                "  Compiled zone: %s\n"
                "  Follows Home Assistant: %s\n"
                "  Chosen zone: %s\n"
                "  In effect: %s",
                this->default_name_, YESNO(this->follow_homeassistant_),
                !this->has_chosen_zone()     ? "none"
                : this->chosen_.name.empty() ? this->chosen_.posix.c_str()
                                             : this->chosen_.name.c_str(),
                zone_source_name(this->zone_source()));
}

bool TimezoneSettings::homeassistant_zone_known_() const {
  const auto &zone = *this->homeassistant_zone_;
  return this->homeassistant_zone_received_ || zone.std_offset_seconds != 0 || zone.has_dst();
}

ZoneSource TimezoneSettings::zone_source() const {
  if (!this->follow_homeassistant_)
    return ZoneSource::MANUAL;
  return this->homeassistant_zone_known_() ? ZoneSource::HOMEASSISTANT : ZoneSource::DEFAULT;
}

const time::ParsedTimezone &TimezoneSettings::zone_in_effect() const {
  switch (this->zone_source()) {
    case ZoneSource::MANUAL:
      return this->chosen_parsed_;
    case ZoneSource::HOMEASSISTANT:
      return *this->homeassistant_zone_;
    case ZoneSource::DEFAULT:
    default:
      return this->default_;
  }
}

void TimezoneSettings::apply_() { time::set_global_tz(this->zone_in_effect()); }

SetError TimezoneSettings::check_zone(const Zone &zone, time::ParsedTimezone &parsed) {
  if (zone.name.size() > ZONE_NAME_MAX)
    return SetError::NAME_TOO_LONG;
  for (const char c : zone.name) {
    // A NUL would end the stored name early; the rest have no business in a zone's name.
    if (static_cast<uint8_t>(c) < 0x20 || c == 0x7F)
      return SetError::BAD_NAME;
  }
  if (zone.posix.size() > POSIX_MAX)
    return SetError::POSIX_TOO_LONG;
  if (!parse_posix_tz(zone.posix, parsed))
    return SetError::BAD_POSIX;
  return SetError::NONE;
}

SetError TimezoneSettings::configure(bool follow_homeassistant, const Zone *zone) {
  // An answer whose zone is not read yet is read in the mode it came in, before this changes
  // the zone in force: read after, it would take this zone for Home Assistant's.
  if (this->sync_pending_) {
    this->cancel_defer(HOMEASSISTANT_ZONE_READ);
    this->after_homeassistant_sync_();
  }
  time::ParsedTimezone parsed = this->chosen_parsed_;
  if (zone != nullptr) {
    if (const SetError error = check_zone(*zone, parsed); error != SetError::NONE)
      return error;
  }
  const Zone &chosen = zone != nullptr ? *zone : this->chosen_;
  if (!follow_homeassistant && chosen.posix.empty())
    return SetError::NO_ZONE;

  StoredSettings stored{};
  stored.follow_homeassistant = follow_homeassistant ? 1 : 0;
  std::memcpy(stored.name, chosen.name.data(), chosen.name.size());
  std::memcpy(stored.posix, chosen.posix.data(), chosen.posix.size());
  const bool changed = follow_homeassistant != this->follow_homeassistant_ || chosen.name != this->chosen_.name ||
                       chosen.posix != this->chosen_.posix;
  if (changed && !this->store_(stored)) {
    // The flush reports for every record at once, so the failure may be another one's while
    // this one reached flash; follow what flash holds, which is what the next boot reads.
    if (!this->flash_holds_(stored)) {
      ESP_LOGE(TAG, "Storing the time settings failed; the zone in effect stays");
      return SetError::NOT_STORED;
    }
    ESP_LOGW(TAG, "Flushing flash failed for another record; the time settings are stored all the same");
  }

  const bool resumes_following = follow_homeassistant && !this->follow_homeassistant_;
  this->follow_homeassistant_ = follow_homeassistant;
  if (zone != nullptr) {
    this->chosen_ = *zone;
    this->chosen_parsed_ = parsed;
  }
  this->apply_();
  ESP_LOGI(TAG, "%s; in effect: %s", follow_homeassistant ? "Following Home Assistant" : "Following the chosen zone",
           zone_source_name(this->zone_source()));
  // Home Assistant's zone may have changed meanwhile, even to the chosen one, which no answer
  // could show; ask now rather than at the next poll.
  if (resumes_following && this->homeassistant_clock_ != nullptr)
    this->homeassistant_clock_->update();
  return SetError::NONE;
}

// save() only queues the record on ESP32: sync() is what reaches NVS, so it is the call that
// can fail.
bool TimezoneSettings::store_(const StoredSettings &stored) {
  return this->pref_.save(&stored) && global_preferences->sync();
}

// The flush empties the queue before it reports, so this reads NVS rather than the record still
// waiting in it. Byte for byte: the record is zero-filled before it is written.
bool TimezoneSettings::flash_holds_(const StoredSettings &stored) {
  StoredSettings readback{};
  return this->pref_.load(&readback) && std::memcmp(&readback, &stored, sizeof(StoredSettings)) == 0;
}

// Upstream fires this before it sets the zone the same message carries, so the zone is read
// after it, in a deferred call: named, so a second sync in the same loop pass does not queue a
// second read, and the snapshot is the one from before the first.
void TimezoneSettings::on_homeassistant_sync_() {
  if (!this->sync_pending_) {
    this->before_sync_ = time::get_global_tz();
    this->sync_pending_ = true;
  }
  this->defer(HOMEASSISTANT_ZONE_READ, [this]() { this->after_homeassistant_sync_(); });
}

void TimezoneSettings::after_homeassistant_sync_() {
  this->sync_pending_ = false;
  const time::ParsedTimezone &now = time::get_global_tz();
  // Followed, what is in force after an answer is taken for Home Assistant's zone: one equal to
  // it and none cannot be told apart. Not followed, the chosen one was in force, so only a
  // change shows that a zone came.
  if (this->follow_homeassistant_ || !same_zone(now, this->before_sync_)) {
    *this->homeassistant_zone_ = now;
    this->homeassistant_zone_received_ = true;
    ESP_LOGD(TAG, "Home Assistant's zone taken");
  }
  if (!this->follow_homeassistant_)
    this->apply_();
}

TimezoneSettings::Status TimezoneSettings::status() const {
  Status status{};
  const time::ParsedTimezone &zone = this->zone_in_effect();
  const time_t now = this->now_();
  int32_t posix_offset = zone.std_offset_seconds;
  if (ESPTime::from_epoch_utc(now).is_valid()) {
    status.time = now;
    status.dst = time::is_in_dst(now, zone);
    if (status.dst)
      posix_offset = zone.dst_offset_seconds;
  }
  // POSIX counts west of UTC.
  status.utc_offset = -static_cast<int64_t>(posix_offset);
  return status;
}

std::string TimezoneSettings::zone_label() const {
  if (this->zone_source() == ZoneSource::MANUAL) {
    std::string city = zone_city(this->chosen_.name);
    if (!city.empty())
      return city;
  }
  return utc_offset_text(this->status().utc_offset);
}

}  // namespace esphome::timezone_settings
