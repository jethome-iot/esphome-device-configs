#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/mqtt_config/mqtt_config.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/hal.h"
#ifdef USE_WEBSERVER_SORTING
#include "esphome/components/web_server/web_server.h"
#endif
#include "payload.h"
#include "slot_config.h"

namespace esphome::mqtt_subscriptions {

// What a slot does this boot: OFF runs nothing (disabled, invalid, no room), SUSPENDED has its
// entity but no subscription (the crash guard), WAITING has no message or no broker.
enum class SlotState : uint8_t { OFF, WAITING, OK, ERROR, SUSPENDED };

// MQTT topics read into entities: a fixed number of slots, saved as a JSON file on the user
// storage and set from the dashboard. Each enabled slot is one sensor, binary sensor or text
// sensor, created at boot; changes apply at the next one.
class MqttSubscriptions : public Component {
 public:
  MqttSubscriptions();

  // Before entity settings apply at HARDWARE + 1, so a relay can bind to a slot.
  float get_setup_priority() const override { return setup_priority::HARDWARE + 3.0f; }
  void setup() override;
  void dump_config() override;

  // --- Codegen ---
  void set_config(mqtt_config::MqttConfig *config) { this->config_ = config; }
  void set_storage(filesystem_storage_abstract::FilesystemStorageAbstract *storage) { this->storage_ = storage; }
  void set_folder_path(const char *folder) { this->folder_ = folder; }
  void set_max_slots(uint8_t count) { this->max_slots_ = count; }
  // A unit a Number slot may take, with its index in codegen's string table.
  void add_unit(const char *unit, uint8_t uom_index);
  // "<prefix> 1" … "<prefix> <count>": the names temperature probes take after this setup.
  void reserve_sensor_names(const char *prefix, uint8_t count);
#ifdef USE_WEBSERVER_SORTING
  void set_web_server_sorting(web_server::WebServer *server, uint64_t group, float weight);
#endif

  // --- Device API, loop task ---
  enum class Result : uint8_t { OK, INVALID, STORAGE, NEWER_FILE };
  // 200, 400, 503, 503.
  static int http_status(Result result);
  // GET /mqtt/subscriptions: every slot as saved, with what runs. Reads the file.
  void write_api_json(JsonObject root);
  // POST /mqtt/subscriptions: saves or clears one slot. `message` is the answer either way.
  Result post(JsonObjectConst body, std::string *message, bool *reboot_required);

  // --- Any task ---
  // A saved slot differs from what runs, or the crash guard suspended the slots this boot.
  bool reboot_required() const { return this->pending_any_.load(); }

  // --- Loop task; slots 0-based, out of range is off and empty ---
  size_t max_slots() const { return this->max_slots_; }
  // Has an entity this boot.
  bool active(size_t slot) const;
  // The running name, "" when not active.
  const std::string &name(size_t slot) const;
  // A message arrived this boot.
  bool has_value(size_t slot) const;
  // "21.5 °C", "On" / "Off", the text; "" while the entity has no value.
  std::string value_text(size_t slot) const;
  SlotState state(size_t slot) const;
  // The crash guard keeps every slot off the broker this boot.
  bool suspended() const { return this->suspended_; }

  static const char *state_key(SlotState state);

  // Topics subscribed at once after a connect; the next ones wait until each has its retained
  // value, or until none has come for wave_timeout_ms_.
  static constexpr size_t SUBSCRIBE_WAVE = 4;

 protected:
  // The callback our entries in the client's list carry: its type tells them apart from a
  // command topic of the same name.
  struct Delivery {
    MqttSubscriptions *owner;
    void operator()(const std::string &topic, const std::string &payload) const {
      this->owner->on_message_(topic, payload);
    }
  };

  struct Running {
    EntityBase *entity{nullptr};  // of the slot's kind; nullptr when the slot runs nothing
    std::string off_reason;       // why an enabled slot runs nothing
    bool has_message{false};
    uint32_t last_ms{0};
    std::string raw;
    std::string error;
  };
  // What a stat() of the file saw, to notice a rewrite nobody announced.
  struct Seen {
    bool failed{false};  // stat() failed for another reason than a missing file
    bool exists{false};
    int64_t size{0};
    time_t mtime{0};
    bool operator==(const Seen &other) const {
      return this->failed == other.failed && this->exists == other.exists && this->size == other.size &&
             this->mtime == other.mtime;
    }
  };

  virtual uint32_t now_ms_() const { return millis(); }
  void on_message_(const std::string &topic, const std::string &payload);

  // The client sends its whole list at once on every connect, and 16 retained values overflow its
  // inbound event pool: the slots' topics join that list only once connected, in waves.
  void on_connect_();
  void on_disconnect_();
  void send_wave_();
  void note_answer_(const std::string &topic);
  void drop_from_client_();

  std::string folder_path_() const;
  std::string file_path_() const;
  // Virtual, as stat_file_ and read_bytes_: the host cannot run out of memory on cue.
  virtual bool serialize_(const std::vector<SlotConfig> &slots, std::string &out) const {
    return serialize_file(slots, out);
  }
  // Virtual, as read_bytes_: the host cannot make a filesystem fail on cue.
  virtual Seen stat_file_() const;
  // The file's `size` bytes; false when they could not all be read now.
  virtual bool read_bytes_(const std::string &path, size_t size, std::string &out) const;
  // Reads and parses the file; a broken one is renamed to .bad when `rename_bad`. One that
  // could not be read now (FAILED) is left alone, and so is what was last seen of it.
  SlotFile read_file_(bool rename_bad);
  bool write_file_(const std::vector<SlotConfig> &slots);
  // The slots the next boot would run from what was read; a FAILED read changes nothing.
  void take_saved_(const SlotFile &file);
  void check_file_();
  // The saved slot changes what the next boot runs: a slot that runs nothing now and is not
  // enabled in the file stays as it is, invalid or not.
  bool slot_pending_(size_t index) const;
  void update_pending_();

  void start_slot_(size_t index);
  EntityBase *create_entity_(size_t index, const SlotConfig &slot);
  // Why the slot's name is taken (by another slot, another entity of its kind or a temperature
  // probe), "" when it is not. `others` are the slots to compare with, by index; null and empty
  // ones are skipped.
  std::string name_conflict_(size_t index, const SlotConfig &slot, const std::vector<const SlotConfig *> &others) const;
  bool is_ours_(const EntityBase *entity) const;
  void set_error_(size_t index, const std::string &error);
  const char *file_error_(SlotFile::Status status) const;

  mqtt_config::MqttConfig *config_{nullptr};
  filesystem_storage_abstract::FilesystemStorageAbstract *storage_{nullptr};
  std::string folder_{"mqtt"};
  uint8_t max_slots_{8};
  std::vector<std::string> units_;
  std::vector<uint8_t> unit_indices_;
  std::string reserved_prefix_;
  uint8_t reserved_count_{0};
#ifdef USE_WEBSERVER_SORTING
  web_server::WebServer *web_server_{nullptr};
  uint64_t sorting_group_{0};
  float sorting_weight_{50};
#endif

  std::vector<SlotConfig> active_;  // what this boot was set up from
  std::vector<SlotConfig> saved_;   // what the file holds now, as the next boot would run it
  std::vector<Running> running_;
  bool suspended_{false};
  bool newer_file_{false};   // this boot found newer firmware's file and runs nothing
  std::string boot_notice_;  // a file renamed to .bad this boot, until a save
  Seen seen_;
  uint32_t check_interval_ms_{30000};

  std::vector<std::string> topics_;  // of the running slots, each once, in slot order
  std::vector<bool> answered_;       // a message came since the topic's subscription went out
  size_t wave_begin_{0};             // the wave out now is topics_[wave_begin_, next_topic_)
  size_t next_topic_{0};
  uint32_t wave_timeout_ms_{1000};
  std::atomic<bool> pending_any_{false};
};

extern MqttSubscriptions *global_mqtt_subscriptions;  // NOLINT(cppcoreguidelines-avoid-non-const-global-variables)

}  // namespace esphome::mqtt_subscriptions
