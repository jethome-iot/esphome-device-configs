#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include "esphome/components/time/real_time_clock.h"
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/optional.h"
#include "esphome/core/preferences.h"

namespace esphome::ntp_servers {

// What may be set. The record is fixed-width, so the preference never changes size.
static const size_t MAX_SERVERS = 3;
static const size_t SERVER_NAME_MAX = 63;

struct StoredServers {
  uint8_t fallback;
  // Packed from the first; the rest are empty.
  char servers[MAX_SERVERS][SERVER_NAME_MAX + 1];
};

/// Why a change was refused; NONE when it took effect.
enum class SetError : uint8_t { NONE, TOO_MANY, TOO_LONG, BAD_NAME, REPEATED, NOT_STORED };

/// The text of a refusal, for an API answer and the log; @p item and @p first count from 0, as
/// check_servers() sets them.
std::string set_error_text(SetError error, size_t item = 0, size_t first = 0);
/// Why @p name is no host name or IPv4 address lwIP can ask: TOO_LONG, BAD_NAME, or NONE.
SetError check_server(const std::string &name);

/// The NTP servers the device asks: up to MAX_SERVERS set at run time and kept in a
/// preference, then the SNTP clock's own unless that fallback is off. lwIP gets one at a time:
/// left to itself it stays with whichever answered last, so a server that is back would never be
/// asked again, and it cannot say which one answered.
class NTPServers : public Component {
 public:
  // Right after the SNTP clock's setup, which hands lwIP the compiled servers and starts it.
  float get_setup_priority() const override { return setup_priority::BEFORE_CONNECTION - 1.0f; }
  void setup() override;
  void loop() override;
  void dump_config() override;

  /// The SNTP clock: its syncs are the answers, its update interval the poll.
  void set_clock(time::RealTimeClock *clock);
  /// One of the clock's own servers, in its order; a generated string literal.
  void add_fallback_server(const char *server) { this->fallback_servers_.push_back(server); }
  void set_preference_hash(uint32_t hash) { this->preference_hash_ = hash; }

  const std::vector<std::string> &servers() const { return this->servers_; }
  bool fallback() const { return this->fallback_; }
  const std::vector<const char *> &fallback_servers() const { return this->fallback_servers_; }
  /// Every server in the order they are asked.
  const std::vector<std::string> &in_use() const { return this->in_use_; }
  /// The server being asked, or the one that answered last; nullptr when there is none.
  const char *current() const;

  struct Sync {
    time_t time;  // UTC, as the clock read once the answer set it
    std::string server;
  };
  /// The last answer since boot.
  const optional<Sync> &last_sync() const { return this->last_sync_; }

  /// Why @p servers cannot be set, or NONE. On a refusal @p item is the entry at fault, and for
  /// REPEATED @p first the earlier one it repeats.
  static SetError check_servers(const std::vector<std::string> &servers, size_t &item, size_t &first);
  /// Stores @p servers and @p fallback and asks the first server at once. Stored before used:
  /// anything but NONE leaves the device as it was. Call it on the loop task.
  SetError configure(const std::vector<std::string> &servers, bool fallback);

 protected:
  /// Hands lwIP @p server alone, or stops it on nullptr. Virtual so the host tests can watch it.
  virtual void use_server_(const char *server);
  /// Virtual so the host tests can cut the network.
  virtual bool connected_() const;
  /// millis(); virtual so the tests can move it, past its wrap too.
  virtual uint32_t millis_() const { return millis(); }
  /// The clock; virtual so the tests can set the instant.
  virtual time_t now_() const { return ::time(nullptr); }
  // Puts the record in flash and flushes it; false when the flush reported a failure. Virtual so
  // the host tests can watch a store fail: the backend takes every write.
  virtual bool store_(const StoredServers &stored);
  // Whether flash holds that record now, read back through the preference.
  bool flash_holds_(const StoredServers &stored);

  /// What lwIP may wait between polls, in ms.
  uint32_t poll_ms_() const;
  uint32_t answer_wait_() const;
  void rebuild_();
  void ask_(size_t index);
  void arm_(uint32_t ms, bool to_first);
  void on_answer_();

  std::vector<std::string> servers_;
  bool fallback_{true};
  std::vector<const char *> fallback_servers_;
  std::vector<std::string> in_use_;
  size_t index_{0};
  optional<Sync> last_sync_;
  // Servers asked in a row without an answer: past a whole round, each waits longer.
  size_t misses_{0};
  // What happens once wait_ms_ has passed since armed_at_: back to the first server, or on to the
  // next for want of an answer.
  bool armed_{false};
  bool to_first_{false};
  uint32_t armed_at_{0};
  uint32_t wait_ms_{0};
  bool was_connected_{false};
  time::RealTimeClock *clock_{nullptr};
  ESPPreferenceObject pref_;
  uint32_t preference_hash_{0};
  // The name lwIP is handed: it keeps the pointer, not the text.
  char asking_[SERVER_NAME_MAX + 1]{};
};

}  // namespace esphome::ntp_servers
