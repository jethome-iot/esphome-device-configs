#pragma once

#include <atomic>
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
#include "esphome/components/switch_hold/switch_hold.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/time.h"
#include "gain_sets.h"
#include "hub_climate.h"
#include "relay_claim.h"
#ifdef USE_WEBSERVER_SORTING
#include "esphome/components/web_server/web_server.h"
#endif

namespace esphome::time {
class RealTimeClock;
}  // namespace esphome::time

namespace esphome::climate_hub {

/// What a mutator did, for a caller that answers over HTTP.
struct Result {
  bool ok{false};
  /// The status that fits: 200, or 400, 404, 409, 413, 500, 503 or 507 on failure.
  uint16_t code{200};
  /// On failure, the sentence the editor shows.
  std::string error;
  /// create(): the id the new thermostat got; restore(): the id it brought.
  std::string id;
  /// A 409 over a relay: the id of the thermostat that holds it, or that is enabled, waits and
  /// names it.
  std::string holder;
  /// Saved and enabled but not running, and why: its sensor or a relay is not on the device, or
  /// no climate entity was free.
  std::string warning;
  /// False when the change is live but did not reach flash, so a reboot undoes it.
  bool persisted{true};
  /// set_enabled() with take_over: the ids of the thermostats it stored disabled for the relay,
  /// the running ones first.
  std::vector<std::string> stopped;
  /// The ids of the waiting thermostats that started because the change freed a relay they
  /// name or a climate entity, in the order they started.
  std::vector<std::string> started;
};

/// Thermostats stored as <storage>/<folder>/<id>.json and run on the loop task, each one a
/// climate entity from a pool of max_controllers that setup() registers with App.
///
/// Everything below runs on the loop task. A caller on another task (an ESP-IDF HTTP handler)
/// wraps its whole read, decide and write in one run_on_loop() job.
///
/// It is the firmware's switch_hold::SwitchHolder: a running thermostat holds its relays, and
/// the other writers ask switch_hold before they move one.
class ClimateHub : public Component, public switch_hold::SwitchHolder {
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
  /// The clock a saved gain set's date and default name come from.
  void set_time_source(time::RealTimeClock *rtc) { this->rtc_ = rtc; }
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
  /// Why an enabled thermostat is not running, worded as the `warning` that said so ("not
  /// started: sensor 'temp_3' not found"): what its last start, at boot, a Save, an enable, a
  /// relay it names or a climate entity coming free, failed on. "" when it runs, is disabled or
  /// is not there.
  std::string waiting_reason(const std::string &id) const;
  /// The running thermostat's control state (action, fault, duties, PID terms, sample age),
  /// nullptr when it is not running.
  const ControllerRuntime *runtime(const std::string &id) const;
  /// The id of the running thermostat that holds this relay, or "".
  std::string claimed_by(const std::string &relay_object_id) const;
  /// The name of the running thermostat that holds `sw`, or "".
  std::string holder_of(const switch_::Switch *sw) const override;
  /// What a sensor reads now, NaN when there is no such sensor or its reading is not a finite
  /// number in °C: a stopped thermostat has no entity to ask.
  float sensor_reading(const std::string &sensor_object_id) const;

  /// Adds a thermostat. The draft's id, its presets' keys and its active_preset are ignored: an
  /// id and keys are made from the names, and no preset is active. Refused with 400
  /// (a rule broken; enabled, and its sensor does not report °C), 409 (name taken, relay held by
  /// a running thermostat or named by an enabled one that waits, every id the name gives taken),
  /// 413 (the file would be over CONFIG_MAX_BYTES), 507 (at max_controllers) or 500 (not
  /// written). An enabled one whose sensor or relay is not on the device is saved and waits, as
  /// at boot, with a `warning`.
  Result create(ClimateConfig draft);
  /// Replaces a thermostat's document; the id stays. A running one keeps its entity and every
  /// relay it still names, or stops when the sensor or a relay it now names is not on the
  /// device. A preset keeps its key when the doc brings it back, one the thermostat never gave
  /// out is made again from the name, and the active preset stays while its key is there, the
  /// doc's active_preset ignored; new values for it apply at once. A relay it holds is never
  /// refused, whoever else names it. 404 for an unknown id, 409 for one a newer firmware wrote,
  /// otherwise as create(). A relay or a climate entity the Save frees starts the thermostats
  /// that wait for it.
  /// `revision`, when given, is the one the caller read: the device changed the document since
  /// if it is not the stored one, and the Save is refused with 409. It ends a calibration.
  Result update(const std::string &id, ClimateConfig doc, optional<uint32_t> revision = nullopt);
  /// Brings a thermostat back under the id its document names, as a backup holds it: one with
  /// that id is replaced, as update() replaces it, otherwise one is created with it. The
  /// presets keep their keys and the active preset stays, since rules name them so, and the
  /// mode to go back to is the document's (its own mode's without one). Refused
  /// as create() and update() refuse, and with 400 for an id that is no slug or is `new`, and
  /// 409 for an id a file the boot did not load holds. The doc's revision is ignored: a
  /// replacement moves the stored one on, a new one starts at 0. It ends a calibration.
  Result restore(ClimateConfig doc);
  /// Stops and deletes a thermostat, and starts the thermostats that wait for its relays or for
  /// a climate entity.
  Result remove(const std::string &id);
  /// Starts or stops a thermostat and stores the flag. Enabling is refused as a Save is: 400
  /// for a sensor that does not report °C, 409 naming the thermostat that holds a relay or,
  /// enabled and waiting, names it, unless `take_over`: each of those is then disabled first,
  /// and a 400 comes instead when the sensor or a relay is not on the device, or a 409 when only
  /// waiting ones name it and no climate entity is free, nothing touched.
  /// Otherwise one whose sensor or relay is not on the device is stored enabled and waits, with
  /// a `warning`. A stop or a take-over starts the thermostats that wait for a freed relay,
  /// then those that wait for a freed climate entity, after the one taking over.
  Result set_enabled(const std::string &id, bool enabled, bool take_over = false);
  /// Moves the target, clamped into the visual range, running or not.
  Result set_setpoint(const std::string &id, float value);
  /// Picks the preset with this key, running or not, as Home Assistant would: its target, its
  /// mode if it has one, and the label. 404 for an unknown thermostat or key; `persisted` false
  /// when it changed one a newer firmware wrote, whose file keeps what it had.
  Result apply_preset(const std::string &id, const std::string &key);
  /// Mode off, running or not, as Home Assistant would set it. 404 for an unknown thermostat;
  /// `persisted` as apply_preset().
  Result turn_off(const std::string &id);
  /// Back to the mode it had before it went off, its on_mode(), as turn_off().
  Result turn_on(const std::string &id);

  /// `callback(id)` runs at the end of a create, a removal, and a Save or a restore that added
  /// the thermostat or changed its preset keys: what the automation rules name of it.
  template<typename F> void add_on_change_callback(F &&callback) {
    this->change_callback_.add(std::forward<F>(callback));
  }

  /// Calibrates a running PID thermostat: the relay of its mode goes full below the target and
  /// off above it, AUTOTUNE_NOISEBAND either side, the other one held open, until the swings
  /// give Ku and Pu. `rule` turns them into kp, ki and kd, which that relay then runs on and the
  /// file keeps as its gains, the revision moved on. 404, or 409 for a bang-bang or stopped
  /// thermostat, a newer firmware's file, one calibrating already, in mode off or with a fault;
  /// 400 for a `direction`, when one is given, that is not the mode's.
  Result start_autotune(const std::string &id, optional<Direction> direction, AutotuneRule rule);
  /// Ends the running calibration as cancelled: 404, or 409 when none runs.
  Result cancel_autotune(const std::string &id);
  /// The thermostat's last calibration since boot, running or ended, nullptr for none.
  const AutotuneRun *autotune(const std::string &id) const;

  /// Saves the gains `direction`'s relay has, the mode's relay when none is given, as a new set
  /// in the thermostat's sets file; a calibration that succeeds saves its own. `name`, trimmed,
  /// meets validate_set_name(), "" for the local date and time. 400 for a name, 404, 409 for a
  /// direction without a relay, 507 below GAIN_SET_MIN_FREE_BYTES free, 500 when not written.
  /// `added` gets the set.
  Result add_gain_set(const std::string &id, optional<Direction> direction, const std::string &name,
                      GainSet *added = nullptr);
  /// Renames set `n`, the rest of its line kept: 400, 404 for the thermostat or the set, 500.
  Result rename_gain_set(const std::string &id, uint32_t n, const std::string &name, GainSet *renamed = nullptr);
  /// Removes set `n`; its number is never given again. 404 for the thermostat or the set, 500.
  Result delete_gain_set(const std::string &id, uint32_t n);
  /// Makes set `n`'s gains its relay's and moves the revision on, as a calibration does; a
  /// running PID goes on with them as after a Save. 404 for the thermostat or the set, 409 while
  /// it calibrates, for a newer firmware's file, or a relay the thermostat no longer has.
  /// `persisted` false when the file is still to be written.
  Result apply_gain_set(const std::string &id, uint32_t n);
  /// Adds a set of `rule`'s gains for set `n`'s Ku and Pu, `from` it, and applies it as
  /// apply_gain_set() with `apply`, whose refusals then come before anything is added. 409 for a
  /// set without Ku and Pu, otherwise as add_gain_set().
  Result derive_gain_set(const std::string &id, uint32_t n, AutotuneRule rule, bool apply, const std::string &name,
                         GainSet *added = nullptr);
  /// The thermostat's sets for a listing on another task, read there; nullptr with a 404. While
  /// it is open the hub writes nothing to that file: what it adds waits in RAM until the reader
  /// is gone, and a rename or a delete is 503.
  std::unique_ptr<GainSetReader> open_gain_sets(const std::string &id, Result *result);

  /// The name rules on a trimmed name, with the sentence that says which one broke.
  static bool validate_name(const std::string &name, std::string *error) {
    return climate_hub::validate_name(name, error);
  }
  /// Whether `name` is taken, by another thermostat (same name_key(), or the same object id by
  /// ESPHome's count or Home Assistant's, which it would call a duplicate) or by a climate
  /// declared in YAML (the web server matches climates by name). `exclude_id` never blocks itself.
  bool is_name_taken(const std::string &name, const std::string &exclude_id, std::string *error) const;

  /// The loop task's clock, millis_64(): a thermostat measures a silence of any length on it.
  /// Virtual so tests can move it.
  virtual uint64_t now_ms() const;

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

  /// The end of create() and restore(): `doc`, written already, joins the store and starts.
  /// `verb` heads the log line.
  Result add_(const ClimateConfig &doc, const char *verb);
  /// The end of update() and restore(): `doc`, written already, takes `stored`'s place.
  Result replace_(ClimateConfig *stored, const ClimateConfig &doc, const char *verb);

  bool start_(ClimateConfig *config, std::string *error);
  /// Keeps `error` as why `id` waits, and returns it worded as a `warning`.
  const std::string &note_waiting_(const std::string &id, const std::string &error);
  /// `why` ends a calibration it runs.
  void stop_(Slot *slot, AutotuneEnd why = AutotuneEnd::STOPPED);
  /// Stores and runs the gains the calibration `slot` runs has found.
  void complete_autotune_(Slot *slot);
  bool restart_(Slot *slot, const std::string &previous_name, std::string *error);
  /// Claims every relay `config` names, or none: false, nothing claimed, when one is missing or
  /// another thermostat holds it.
  bool acquire_claims_(const ClimateConfig &config, RelayClaim **heat, RelayClaim **cool, std::string *error);
  void release_claims_(const std::string &owner);
  /// Opens the claim's relay, remembers its last switching and drops the claim.
  ClaimMap::iterator let_go_(ClaimMap::iterator it, uint64_t now_ms);
  /// The end of a mutator, after start_waiters_(): tells switch_hold about each relay let go of
  /// that is still free, and forgets them all.
  void announce_released_();
  /// Moves the claims `from` holds on relays `to` names over to `to`, relays as they are.
  void hand_over_(const std::string &from, Slot *holding, const ClimateConfig &to);
  /// The running thermostat, other than `config` itself, that holds one of its relays, and
  /// which relay.
  std::string holder_of_(const ClimateConfig &config, std::string *relay_id = nullptr) const;
  /// The first by id of the other thermostats that are enabled, wait, and name one of the
  /// relays `config` does not hold already, and which relay.
  std::string reserver_of_(const ClimateConfig &config, std::string *relay_id = nullptr) const;
  /// Whether the sensor and the relays `config` names are on this device, the sensor in °C:
  /// what a take-over asks before it stops the holder.
  bool check_entities_(const ClimateConfig &config, std::string *error) const;
  /// Whether `config`, enabled, may be saved: 400 for a sensor not in °C, 409 for a relay held
  /// elsewhere or reserved by a thermostat that waits. A missing sensor or relay is waited for,
  /// as at boot.
  bool check_savable_(const ClimateConfig &config, Result *result) const;
  /// The 409 naming `holder`, which holds the relay or, `waits`, is enabled and waits for it.
  Result relay_held_(const std::string &relay_id, const std::string &holder, bool waits) const;
  /// Starts the enabled thermostats, `skip_id` aside, that name a relay let go since the last
  /// announce_released_(), then, if a stop freed a climate entity since the last call, the ones
  /// that wait for an entity while one is free, each in id order, and adds the ones that
  /// started to `result`.
  void start_waiters_(const std::string &skip_id, Result *result);
  /// Whether `id`'s last start found no free climate entity.
  bool waits_for_entity_(const std::string &id) const;
  Slot *slot_for_(const std::string &id) const;
  /// The hidden slot that carries `name`, else one with its object id by ESPHome's count or Home
  /// Assistant's; free_.end() for none.
  std::deque<Slot *>::iterator free_slot_like_(const std::string &name);
  /// free_slot_like_(), so a thermostat back under its name gets back its key; otherwise the
  /// one freed longest ago. Only while one is free.
  Slot *take_free_slot_(const std::string &name);
  /// Before `renamed`, running, shows `name`: free_slot_like_() takes the name `renamed` leaves.
  /// The web server answers the first climate by a name, and a listing queued before the slot
  /// was hidden sends its key, so neither may stay with it. Never the placeholder: an API client
  /// may still be encoding that slot.
  void give_way_(const std::string &name, const Slot *renamed);
  SensorSubscription *subscribe_(sensor::Sensor *sensor);
  void on_sample_(SensorSubscription *sub, float value);
  void on_control_(uint8_t index, const climate::ClimateCall &call);
  Result set_mode_(const std::string &id, HubMode mode);

  /// A free id made from `name`, "" when every one is taken.
  std::string next_id_(const std::string &name) const;
  std::string folder_() const;
  std::string file_path_(const std::string &id) const;
  bool ensure_folder_();
  /// False, nothing written, when the write failed, the next boot could not load the file or a
  /// newer firmware wrote it; `too_large` then says whether it was the size.
  bool save_(const ClimateConfig &config, bool *too_large = nullptr);
  // Seam: the rules keep every document under the cap, and no host fails one allocation.
  virtual EncodeError encode_(const ClimateConfig &config, std::string *json) const { return config.encode(json); }
  /// False when the file is still there to be loaded at the next boot.
  bool delete_file_(const std::string &id);
  // Seam: no host filesystem lets a test refuse one unlink.
  virtual bool remove_file_(const std::string &path);

  void mark_dirty_(const std::string &id);
  void flush_dirty_();

  /// A set to add, or a sets file to delete, once no reader has the file open.
  struct PendingSets {
    std::string id;
    bool drop;
    GainSet set;
  };
  std::string gains_folder_() const;
  std::string gain_sets_path_(const std::string &id) const;
  bool ensure_gains_folder_();
  /// Numbers `set`, dates it, names it `name` or by its `label`, and adds it: false with
  /// `result` set when it was refused.
  bool append_set_(const std::string &id, const std::string &name, const char *label, GainSet *set, Result *result);
  /// The set `n`, the ones waiting in RAM too.
  bool find_set_(const std::string &id, uint32_t n, GainSet *set);
  /// A rename or a delete rewrites the file: none while a reader has it open, 503 then.
  bool settle_sets_(Result *result);
  /// apply_gain_set()'s 409s.
  bool may_apply_(const ClimateConfig &stored, Direction direction, Result *result) const;
  Result apply_set_(ClimateConfig *stored, const GainSet &set);
  /// Saves a calibration's result as a set, and logs why when it cannot.
  void save_calibration_(const std::string &id, AutotuneRun *run);
  /// Deletes the thermostat's sets file, once no reader has it open.
  void drop_gain_sets_(const std::string &id);
  void delete_gain_file_(const std::string &id);
  void flush_pending_sets_();
  // Seams: tests set the clock and the free space.
  virtual ESPTime clock_now_() const;
  virtual filesystem_storage_abstract::StorageInfo storage_info_() const { return this->storage_->get_storage_info(); }
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
  // Why each enabled thermostat that is not running did not start, by id, as a `warning`.
  std::map<std::string, std::string> waiting_;
  // Relays let go since the last announce_released_(), by object id.
  std::map<std::string, switch_::Switch *> freed_;
  // A stop handed a climate entity back since the last start_waiters_().
  bool entity_freed_{false};
  // Each thermostat's last calibration, by id, in RAM: a reboot forgets them.
  std::map<std::string, std::unique_ptr<AutotuneRun>> autotunes_;
  // One per sensor, kept for the life of the device: upstream has no callback removal.
  std::vector<std::unique_ptr<SensorSubscription>> sensor_subs_;

  CallbackManager<void(const std::string &)> change_callback_;

  std::set<std::string> dirty_;
  uint64_t dirty_since_ms_{0};
  time::RealTimeClock *rtc_{nullptr};
  // Loop task only; flushed by the loop once no reader is open, and at shutdown.
  std::vector<PendingSets> pending_sets_;
  // Readers open on the web server's task, which closes them.
  std::atomic<uint8_t> gain_readers_{0};
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
