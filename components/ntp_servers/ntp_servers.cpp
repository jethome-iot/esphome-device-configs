#include "ntp_servers.h"
#include <algorithm>
#include <cstring>
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

#ifdef USE_NETWORK
#include "esphome/components/network/util.h"
#endif
#ifdef USE_ESP32
#include "esp_sntp.h"
#include "esphome/components/sntp/sntp_component.h"
#endif

namespace esphome::ntp_servers {

static const char *const TAG = "ntp_servers";
// How long a server has to answer before the next is asked: lwIP's first request goes out up to
// 5 s after it starts, then waits 15 s for the reply.
static const uint32_t ANSWER_MS = 30000;
// ESP-IDF polls no more often than that.
static const uint32_t MIN_POLL_MS = 15000;
// How far ahead of lwIP's own poll the first server is asked again: lwIP times its poll from the
// reply, which the loop task hears of later. Below MIN_POLL_MS.
static const uint32_t RETURN_LEAD_MS = 5000;
// Past a whole round with no answer, the wait doubles up to this many times, never past a poll.
static const size_t MAX_BACKOFF = 5;

std::string set_error_text(SetError error, size_t item, size_t first) {
  switch (error) {
    case SetError::TOO_MANY:
      return str_sprintf("'ntp.servers' lists more than %zu servers", MAX_SERVERS);
    case SetError::TOO_LONG:
      return str_sprintf("'ntp.servers' item %zu is over %zu bytes", item + 1, SERVER_NAME_MAX);
    case SetError::BAD_NAME:
      return str_sprintf("'ntp.servers' item %zu is not a host name or an IPv4 address", item + 1);
    case SetError::REPEATED:
      return str_sprintf("'ntp.servers' item %zu repeats item %zu", item + 1, first + 1);
    case SetError::NOT_STORED:
      return "The NTP servers were not stored";
    case SetError::NONE:
    default:
      return "";
  }
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }
static bool is_alnum(char c) { return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

SetError check_server(const std::string &name) {
  if (name.size() > SERVER_NAME_MAX)
    return SetError::TOO_LONG;
  size_t labels = 0;
  bool numeric = true;
  bool octets = true;
  size_t start = 0;
  while (true) {
    const size_t dot = name.find('.', start);
    const size_t end = dot == std::string::npos ? name.size() : dot;
    if (end == start || name[start] == '-' || name[end - 1] == '-')
      return SetError::BAD_NAME;
    bool digits = true;
    unsigned value = 0;
    for (size_t i = start; i < end; i++) {
      if (!is_alnum(name[i]) && name[i] != '-')
        return SetError::BAD_NAME;
      digits = digits && is_digit(name[i]);
      if (digits && value <= 255)
        value = value * 10 + (name[i] - '0');
    }
    if (!digits) {
      numeric = false;
    } else if (value > 255 || (end - start > 1 && name[start] == '0')) {
      // A leading zero reads as octal to lwIP.
      octets = false;
    }
    labels++;
    if (dot == std::string::npos)
      break;
    start = dot + 1;
  }
  // All digits is an address, whatever DNS would make of it.
  if (numeric && (labels != 4 || !octets))
    return SetError::BAD_NAME;
  return SetError::NONE;
}

SetError NTPServers::check_servers(const std::vector<std::string> &servers, size_t &item, size_t &first) {
  item = 0;
  first = 0;
  if (servers.size() > MAX_SERVERS)
    return SetError::TOO_MANY;
  for (item = 0; item < servers.size(); item++) {
    if (const SetError error = check_server(servers[item]); error != SetError::NONE)
      return error;
    for (first = 0; first < item; first++) {
      if (str_equals_case_insensitive(servers[first], servers[item]))
        return SetError::REPEATED;
    }
  }
  return SetError::NONE;
}

void NTPServers::set_clock(time::RealTimeClock *clock) {
  this->clock_ = clock;
  clock->add_on_time_sync_callback([this]() { this->on_answer_(); });
}

const char *NTPServers::current() const {
  return this->index_ < this->in_use_.size() ? this->in_use_[this->index_].c_str() : nullptr;
}

#ifdef USE_ESP32
NTPServers *NTPServers::instance_ = nullptr;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

// Upstream's own callback, plus the server: on lwIP's task, its lock held, slot 0 still names the
// one whose reply this is.
void NTPServers::on_reply_(struct timeval * /*tv*/) {
  const char *name = esp_sntp_getservername(0);
  std::string server = name != nullptr ? name : "";
  instance_->defer([server]() {
    instance_->answered_by_(server);
    static_cast<sntp::SNTPComponent *>(instance_->clock_)->time_synced();
  });
}
#endif

void NTPServers::setup() {
#ifdef USE_ESP32
  instance_ = this;
  esp_sntp_set_time_sync_notification_cb(&NTPServers::on_reply_);
#endif
  this->pref_ = global_preferences->make_preference<StoredServers>(this->preference_hash_);
  StoredServers stored{};
  // Nothing stored: the clock's own servers, as before there was a choice.
  if (this->pref_.load(&stored)) {
    this->fallback_ = stored.fallback != 0;
    for (auto &entry : stored.servers) {
      entry[SERVER_NAME_MAX] = '\0';
      if (entry[0] == '\0')
        continue;
      std::vector<std::string> with = this->servers_;
      with.emplace_back(entry);
      size_t item, first;
      if (check_servers(with, item, first) == SetError::NONE) {
        this->servers_ = std::move(with);
      } else {
        ESP_LOGE(TAG, "The stored server '%s' is refused; dropped", entry);
      }
    }
  }
  this->rebuild_();
  this->was_connected_ = this->connected_();
  this->ask_(0);
}

void NTPServers::dump_config() {
  std::string servers;
  for (const auto &server : this->servers_)
    servers += (servers.empty() ? "" : ", ") + server;
  std::string fallback;
  for (const char *server : this->fallback_servers_)
    fallback += std::string(fallback.empty() ? "" : ", ") + server;
  const char *current = this->current();
  ESP_LOGCONFIG(TAG,
                "NTP servers:\n"
                "  Set: %s\n"
                "  Fallback: %s (%s)\n"
                "  Asking: %s",
                servers.empty() ? "none" : servers.c_str(), YESNO(this->fallback_),
                fallback.empty() ? "none" : fallback.c_str(), current == nullptr ? "none" : current);
}

// The set servers, then the clock's own that are not among them already.
void NTPServers::rebuild_() {
  this->in_use_ = this->servers_;
  if (!this->fallback_)
    return;
  for (const char *server : this->fallback_servers_) {
    const bool listed = std::any_of(this->in_use_.begin(), this->in_use_.end(), [server](const std::string &name) {
      return str_equals_case_insensitive(name, server);
    });
    if (!listed)
      this->in_use_.emplace_back(server);
  }
}

uint32_t NTPServers::poll_ms_() const {
  const uint32_t interval = this->clock_ != nullptr ? this->clock_->get_update_interval() : 0;
  return std::max(interval, MIN_POLL_MS);
}

uint32_t NTPServers::answer_wait_() const {
  const size_t rounds = this->misses_ / std::max<size_t>(this->in_use_.size(), 1);
  const uint32_t wait = ANSWER_MS << std::min(rounds, MAX_BACKOFF);
  return std::max(ANSWER_MS, std::min(wait, this->poll_ms_()));
}

void NTPServers::arm_(uint32_t ms, bool to_first) {
  this->armed_ = true;
  this->to_first_ = to_first;
  this->armed_at_ = this->millis_();
  this->wait_ms_ = ms;
}

void NTPServers::ask_(size_t index) {
  this->index_ = index;
  this->armed_ = false;
  const char *server = this->current();
  this->use_server_(server);
  if (server != nullptr)
    ESP_LOGD(TAG, "Asking %s", server);
  // One server is lwIP's to retry.
  if (this->in_use_.size() > 1)
    this->arm_(this->answer_wait_(), false);
}

void NTPServers::loop() {
  if (this->in_use_.empty())
    return;
  // Offline nothing can answer; back online, the first is asked again at once, as at boot.
  const bool connected = this->connected_();
  if (!connected || !this->was_connected_) {
    if (connected) {
      this->misses_ = 0;
      this->ask_(0);
    }
    this->was_connected_ = connected;
    return;
  }
  // Unsigned, so a millis() wrap in between counts right.
  if (!this->armed_ || this->millis_() - this->armed_at_ < this->wait_ms_)
    return;
  if (this->to_first_) {
    this->ask_(0);
    return;
  }
  this->misses_++;
  const size_t next = (this->index_ + 1) % this->in_use_.size();
  ESP_LOGW(TAG, "No answer from %s; asking %s", this->current(), this->in_use_[next].c_str());
  this->ask_(next);
}

void NTPServers::on_answer_() {
  // Upstream's first loop pass reports a clock that was already set, after a restart.
  if (!this->answer_.has_value())
    return;
  std::string server = std::move(*this->answer_);
  this->answer_.reset();
  // A reply lwIP processed as its slot was cleared has nobody to credit.
  if (server.empty())
    return;
  this->last_sync_ = Sync{this->now_(), server};
  // From a server asked before a change or a move on: it says nothing of the one asked now.
  const char *current = this->current();
  if (current == nullptr || server != current)
    return;
  this->misses_ = 0;
  if (this->in_use_.size() < 2) {
    this->armed_ = false;
    return;
  }
  const uint32_t poll = this->poll_ms_();
  if (this->index_ == 0) {
    // Its next poll answered, nothing happens; missed, the next server is asked.
    this->arm_(poll > UINT32_MAX - ANSWER_MS ? UINT32_MAX : poll + ANSWER_MS, false);
  } else {
    // Instead of this one's next poll: the first may be back.
    this->arm_(poll - RETURN_LEAD_MS, true);
  }
}

SetError NTPServers::configure(const std::vector<std::string> &servers, bool fallback) {
  size_t item, first;
  if (const SetError error = check_servers(servers, item, first); error != SetError::NONE)
    return error;
  if (servers == this->servers_ && fallback == this->fallback_)
    return SetError::NONE;
  StoredServers stored{};
  stored.fallback = fallback ? 1 : 0;
  for (size_t i = 0; i < servers.size(); i++)
    std::memcpy(stored.servers[i], servers[i].data(), servers[i].size());
  if (!this->store_(stored)) {
    // The flush reports for every record at once, so the failure may be another one's while
    // this one reached flash; follow what flash holds, which is what the next boot reads.
    if (!this->flash_holds_(stored)) {
      ESP_LOGE(TAG, "Storing the NTP servers failed; the ones in use stay");
      return SetError::NOT_STORED;
    }
    ESP_LOGW(TAG, "Flushing flash failed for another record; the NTP servers are stored all the same");
  }
  this->servers_ = servers;
  this->fallback_ = fallback;
  this->rebuild_();
  this->misses_ = 0;
  ESP_LOGI(TAG, "%zu NTP server(s) set, fallback %s", servers.size(), ONOFF(fallback));
  this->ask_(0);
  return SetError::NONE;
}

// save() only queues the record on ESP32: sync() is what reaches NVS, so it is the call that
// can fail.
bool NTPServers::store_(const StoredServers &stored) { return this->pref_.save(&stored) && global_preferences->sync(); }

// The flush empties the queue before it reports, so this reads NVS rather than the record still
// waiting in it. Byte for byte: the record is zero-filled before it is written.
bool NTPServers::flash_holds_(const StoredServers &stored) {
  StoredServers readback{};
  return this->pref_.load(&readback) && std::memcmp(&readback, &stored, sizeof(StoredServers)) == 0;
}

bool NTPServers::connected_() const {
#ifdef USE_NETWORK
  return network::is_connected();
#else
  return true;
#endif
}

void NTPServers::use_server_(const char *server) {
#ifdef USE_ESP32
  // Stop and init are queued for lwIP's task in this order; the slot calls between them take its
  // lock, so the restart finds the slots set.
  esp_sntp_stop();
  // Every slot: the clock's setup filled one per compiled server, and lwIP moves on to any left.
  for (uint8_t i = 0; i < CONFIG_LWIP_SNTP_MAX_SERVERS; i++)
    esp_sntp_setserver(i, nullptr);
  if (server == nullptr)
    return;
  // No slot points into the string any more.
  this->asking_ = server;
  esp_sntp_setservername(0, this->asking_.c_str());
  esp_sntp_init();
#else
  (void) server;
#endif
}

}  // namespace esphome::ntp_servers
