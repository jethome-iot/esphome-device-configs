#pragma once

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "climate_config.h"
#include "config_store.h"
#include "controller_runtime.h"
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/components/loop_job/loop_job.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "hub_climate.h"
#include "relay_claim.h"
#ifdef USE_WEBSERVER_SORTING
#include "esphome/components/web_server/web_server.h"
#endif

namespace esphome::climate_hub {

/// What a mutator did, for a caller that answers over HTTP.
struct Result {
  bool ok{false};
  /// The status that fits: 200, or 400, 404, 409, 413, 500 or 507 on failure.
  uint16_t code{200};
  /// On failure, the sentence the editor shows.
  std::string error;
  /// create(): the id the new thermostat got.
  std::string id;
  /// A 409 over a relay: the id of the running thermostat that holds it.
  std::string holder;
  /// Saved and enabled but not running, and why: no climate entity was free.
  std::string warning;
  /// False when the change is live but did not reach flash, so a reboot undoes it.
  bool persisted{true};
};

/// Thermostats stored as <storage>/<folder>/<id>.json and run on the loop task, each one a
/// climate entity from a pool of max_controllers that setup() registers with App.
///
/// Everything below runs on the loop task. A caller on another task (an ESP-IDF HTTP handler)
/// wraps its whole read, decide and write in one run_on_loop() job.
class ClimateHub : public Component {
 public:
  ClimateHub();

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override;
  // After dallas_scan (DATA) has created the Temp N sensors, before automations (DATA - 1).
  float get_setup_priority() const override { return setup_priority::DATA - 0.5f; }

  void set_storage(filesystem_storage_abstract::FilesystemStorageAbstract *storage) { this->storage_ = storage; }
  void set_folder_path(const std::string &path) { this->folder_path_ = path; }
  void set_max_controllers(uint8_t count) { this->max_controllers_ = count; }
  void set_icon_index(uint8_t index) { this->entity_fields_ = static_cast<uint32_t>(index) << ENTITY_FIELD_ICON_SHIFT; }
#ifdef USE_WEBSERVER_SORTING
  void set_web_server_sorting(web_server::WebServer *server, uint64_t group, float weight);
#endif

  /// Runs `job` on the loop task and blocks until it answers. False when the loop never got to
  /// it: then the job did not run and never will. Virtual so tests can count what crosses.
  virtual bool run_on_loop(std::function<bool()> &&job);

  /// The documents, sorted by id.
  const ConfigStore &store() const { return this->store_; }
  uint8_t max_controllers() const { return this->max_controllers_; }
  /// Whether the thermostat is running: enabled, and its sensor and relays were there.
  bool is_running(const std::string &id) const { return this->slot_for_(id) != nullptr; }
  /// The running thermostat's control state (action, fault, duties, PID terms, sample age),
  /// nullptr when it is not running.
  const ControllerRuntime *runtime(const std::string &id) const;
  /// The id of the running thermostat that holds this relay, or "".
  std::string claimed_by(const std::string &relay_object_id) const;
  /// What a sensor reads now, NaN when there is no such sensor or no reading: a stopped
  /// thermostat has no entity to ask.
  float sensor_reading(const std::string &sensor_object_id) const;

  /// Adds a thermostat. The draft's id is ignored: one is made from the name. Refused with 400
  /// (a rule broken; enabled, and its sensor or a relay is not on the device, or its sensor does
  /// not report °C), 409 (name taken, relay held by a running thermostat, every id the name
  /// gives taken), 413 (the file would be over CONFIG_MAX_BYTES), 507 (at max_controllers) or
  /// 500 (not written).
  Result create(ClimateConfig draft);
  /// Replaces a thermostat's document; the id stays. A running one keeps its entity and every
  /// relay it still names. 404 for an unknown id, otherwise as create().
  Result update(const std::string &id, ClimateConfig doc);
  /// Stops and deletes a thermostat.
  Result remove(const std::string &id);
  /// Starts or stops a thermostat and stores the flag. Enabling one whose sensor or relay is not
  /// on the device, or whose sensor does not report °C, is a 400; one whose relay a running
  /// thermostat holds is a 409 naming the holder, unless `take_over`: the holder is then
  /// disabled first.
  Result set_enabled(const std::string &id, bool enabled, bool take_over = false);
  /// Moves the target, clamped into the visual range, running or not.
  Result set_setpoint(const std::string &id, float value);

  /// The name rules on a trimmed name, with the sentence that says which one broke.
  static bool validate_name(const std::string &name, std::string *error) {
    return climate_hub::validate_name(name, error);
  }
  /// Whether `name` is taken, by another thermostat (same name_key() or same object id, which
  /// Home Assistant would call a duplicate) or by a climate declared in YAML (the web server
  /// matches climates by name). `exclude_id` never blocks itself.
  bool is_name_taken(const std::string &name, const std::string &exclude_id, std::string *error) const;

  /// The loop task's clock. Virtual so tests can move it.
  virtual uint32_t now_ms() const;

 protected:
  friend class HubClimate;

  // One pool entity with the control loop that drives it, allocated in setup() and never freed.
  struct Slot {
    Slot(ClimateHub *hub, uint8_t index) : entity(hub, index), runtime(&entity) {}
    HubClimate entity;
    ControllerRuntime runtime;
  };
  struct SensorSubscription {
    ClimateHub *hub;
    sensor::Sensor *sensor;
    // What a thermostat that starts on this sensor goes by: the sensor's own state may be old.
    Reading last;
  };
  using ClaimMap = std::map<std::string, std::unique_ptr<RelayClaim>>;

  void build_pool_();
  bool load_();
  bool load_file_(const std::string &folder, const std::string &filename);
  void resolve_name_(ClimateConfig *config);

  bool start_(ClimateConfig *config, std::string *error);
  void stop_(Slot *slot);
  bool restart_(Slot *slot, const std::string &previous_name, std::string *error);
  bool acquire_claims_(const ClimateConfig &config, RelayClaim **heat, RelayClaim **cool, std::string *error);
  void release_claims_(const std::string &owner);
  /// Opens the claim's relay, remembers its last switching and drops the claim.
  ClaimMap::iterator let_go_(ClaimMap::iterator it, uint32_t now_ms);
  /// Moves the claims `from` holds on relays `to` names over to `to`, relays as they are.
  void hand_over_(const std::string &from, Slot *holding, const ClimateConfig &to);
  /// The running thermostat, other than `config` itself, that holds one of its relays, and
  /// which relay.
  std::string holder_of_(const ClimateConfig &config, std::string *relay_id = nullptr) const;
  /// Whether the sensor and the relays `config` names are on this device, the sensor in °C.
  bool check_entities_(const ClimateConfig &config, std::string *error) const;
  /// Whether `config` could run now: 400 for a missing entity, 409 for a relay held elsewhere.
  bool check_startable_(const ClimateConfig &config, Result *result) const;
  Result relay_held_(const std::string &relay_id, const std::string &holder) const;
  Slot *slot_for_(const std::string &id) const;
  /// The hidden slot that last carried `name` (by object id), so a thermostat back under its
  /// name gets back its key; otherwise the one freed longest ago.
  Slot *take_free_slot_(const std::string &name);
  /// Parks every hidden slot but `keep` whose name `name` is about to take: the web server
  /// answers the first climate that matches, hidden or not.
  void park_names_like_(const std::string &name, const Slot *keep);
  SensorSubscription *subscribe_(sensor::Sensor *sensor);
  void on_sample_(SensorSubscription *sub, float value);
  void on_control_(uint8_t index, const climate::ClimateCall &call);

  /// A free id made from `name`, "" when every one is taken.
  std::string next_id_(const std::string &name) const;
  std::string folder_() const;
  std::string file_path_(const std::string &id) const;
  bool ensure_folder_();
  /// False, nothing written, when the write failed or the next boot could not load the file;
  /// `too_large` then says whether it was the size.
  bool save_(const ClimateConfig &config, bool *too_large = nullptr);
  // Seam: the rules keep every document under the cap, and no host fails one allocation.
  virtual EncodeError encode_(const ClimateConfig &config, std::string *json) const { return config.encode(json); }
  /// False when the file is still there to be loaded at the next boot.
  bool delete_file_(const std::string &id);
  // Seam: no host filesystem lets a test refuse one unlink.
  virtual bool remove_file_(const std::string &path);

  void mark_dirty_(const std::string &id);
  void flush_dirty_();
  /// After a thermostat appeared, went, or changed its name or traits: Home Assistant lists
  /// entities only when it connects. Named, so a burst of edits costs one reconnect.
  void schedule_ha_resync_();
  // Seam: tests count the reconnects instead of running an API server.
  virtual void resync_home_assistant_();
  bool refuse_if_failed_(Result *result) const;

  filesystem_storage_abstract::FilesystemStorageAbstract *storage_{nullptr};
  std::string folder_path_{"climates"};
  uint8_t max_controllers_{8};
  uint32_t entity_fields_{0};

  ConfigStore store_;
  std::vector<Slot *> slots_;
  // First in, first out: a web server call resolved just before a slot was freed is unlikely
  // to find it taken again.
  std::deque<Slot *> free_;
  // Keyed on the relay's object id; the owner is the id of the running thermostat.
  ClaimMap claims_;
  // Each relay's last switching once its claim is gone, by object id: the next claim on it
  // honours min_on and min_off from there.
  std::map<std::string, RelaySwitching> relay_history_;
  // One per sensor, kept for the life of the device: upstream has no callback removal.
  std::vector<std::unique_ptr<SensorSubscription>> sensor_subs_;

  std::set<std::string> dirty_;
  uint32_t dirty_since_ms_{0};
  uint32_t ha_resync_delay_ms_{2000};
  loop_job::LoopDispatcher dispatcher_;
#ifdef USE_WEBSERVER_SORTING
  web_server::WebServer *web_server_{nullptr};
  uint64_t sorting_group_{0};
  float sorting_weight_{50};
#endif
};

}  // namespace esphome::climate_hub

namespace esphome {
extern climate_hub::ClimateHub *global_climate_hub;  // NOLINT
}  // namespace esphome
