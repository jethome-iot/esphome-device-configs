#pragma once

// The API an HTTP layer uses. Everything runs on the loop task: a caller on another task wraps
// its whole read, decide and write in one run_on_loop(job), as for climate_hub.
//
//   run_on_loop(job), is_failed(), max_schedules()
//   Reads, for the list and the editor:
//     schedules(), get(id)            the documents, sorted by id; document(id) as the JSON a Save takes
//     target_name(config)             the relay's or the thermostat's name, its id when it is not there
//     status(id)                      ScheduleStatus: state, reason, held_by, note
//     now_and_next(config, &a, &b)    the points in force and next, false while the clock is not valid;
//                                     PointRef's point goes out through config.write_value(obj, *point)
//     clock_text(&day, &time)         the device's local "wed", "10:15:42", false while not valid
//     relay_targets(), thermostat_targets()   what a schedule may drive, who holds and who schedules it
//   Writes, each a Result:
//     create(draft), update(id, doc, revision), restore(doc), remove(id), set_enabled(id, enabled, take_over)

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/components/loop_job/loop_job.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/optional.h"
#include "esphome/core/time.h"
#include "schedule_config.h"

namespace esphome {

namespace time {
class RealTimeClock;
}
// A config without a switch: section has no switch sources; the name must still exist.
namespace switch_ {
class Switch;
}

namespace schedules {

/// What a mutator did, for a caller that answers over HTTP.
struct Result {
  bool ok{false};
  /// The status that fits: 200, or 400, 404, 409, 413, 500 or 507 on failure.
  uint16_t code{200};
  /// On failure, the sentence the editor shows.
  std::string error;
  /// create(): the id the new schedule got; restore(): the id it brought.
  std::string id;
  /// A 409 over a target: the id of the schedule that drives it.
  std::string holder;
  /// Saved enabled but not running, and why: "not running: relay \"relay_7\" is not on this device".
  std::string warning;
  /// False when the change is live but did not reach flash, so a reboot undoes it.
  bool persisted{true};
  /// set_enabled() with take_over: the ids of the schedules it stored disabled, by id.
  std::vector<std::string> stopped;
  /// The ids of the schedules that started driving their target because the change let them.
  std::vector<std::string> started;
};

enum class ScheduleState : uint8_t {
  DISABLED,
  WAITING_FOR_CLOCK,
  MISSING_TARGET,
  MISSING_PRESET,
  /// Enabled, but another enabled schedule, the first by id, drives its target.
  CONFLICT,
  /// Runs, but a thermostat holds its relay right now.
  HELD,
  RUNNING,
};

const char *state_to_string(ScheduleState state);

struct ScheduleStatus {
  ScheduleState state{ScheduleState::DISABLED};
  /// Why it does not drive its target, "" when it runs or is disabled.
  std::string reason;
  /// HELD: the name of the thermostat holding the relay.
  std::string held_by;
  /// No fault, but worth saying: the thermostat it drives is stopped.
  std::string note;
};

/// A switch point placed in the week.
struct PointRef {
  /// 0 is Monday, as in DAY_NAMES.
  uint8_t day{0};
  /// Minutes since midnight.
  uint16_t minute{0};
  const Point *point{nullptr};
};

struct RelayTarget {
  std::string object_id;
  std::string name;
  /// The thermostat that holds it now, "".
  std::string held_by;
  /// The id of the enabled schedule that drives it, "".
  std::string scheduled_by;
};

struct PresetTarget {
  std::string key;
  std::string name;
  float setpoint{NAN};
  /// A mode word, or "keep".
  std::string mode;
};

struct ThermostatTarget {
  std::string id;
  std::string name;
  bool enabled{false};
  bool running{false};
  std::string mode;
  float min_temperature{NAN};
  float max_temperature{NAN};
  float step{NAN};
  std::string scheduled_by;
  std::vector<PresetTarget> presets;
};

/// Weekly schedules stored as <storage>/<folder>/<id>.json. Each one drives a relay or a
/// climate_hub thermostat: at every switch point its target takes the point's value, and
/// whenever the schedule starts it takes the value in force.
class ScheduleEngine : public Component {
 public:
  void setup() override;
  void dump_config() override;
  // After the relays, the thermostats (DATA - 0.5), bindings (DATA - 0.75) and automations (DATA - 1).
  float get_setup_priority() const override { return setup_priority::DATA - 1.5f; }

  void set_storage(filesystem_storage_abstract::FilesystemStorageAbstract *storage) { this->storage_ = storage; }
  void set_time_source(time::RealTimeClock *rtc) { this->rtc_ = rtc; }
  void set_folder_path(const std::string &path) { this->folder_path_ = path; }
  void set_max_schedules(uint8_t count) { this->max_schedules_ = count; }

  /// Runs `job` on the loop task and blocks until it answers. False when the loop never got to
  /// it: then the job did not run and never will. Virtual so tests can count what crosses.
  virtual bool run_on_loop(std::function<bool()> &&job);

  uint8_t max_schedules() const { return this->max_schedules_; }
  /// The documents, sorted by id.
  std::vector<const ScheduleConfig *> schedules() const;
  const ScheduleConfig *get(const std::string &id) const;
  /// The document as its file holds it, "" for an unknown id.
  std::string document(const std::string &id) const;
  /// Where the schedule stands, checked now.
  ScheduleStatus status(const std::string &id) const;
  /// The relay's or the thermostat's name; its id when it is not on the device.
  std::string target_name(const ScheduleConfig &config) const;
  /// The point in force and the next one, the next even with the same value. False while the
  /// clock has not been valid yet.
  bool now_and_next(const ScheduleConfig &config, PointRef *now, PointRef *next) const;
  /// The device's local day ("wed") and time ("10:15:42"); false while the clock is not valid.
  bool clock_text(std::string *day, std::string *time);
  /// Every visible switch.
  std::vector<RelayTarget> relay_targets() const;
  /// Every thermostat, none without climate_hub.
  std::vector<ThermostatTarget> thermostat_targets() const;
  /// The id of the enabled schedule that drives the target, the first by id; "" for none.
  std::string scheduled_by(TargetType type, const std::string &id) const;

  /// Adds a schedule; its id is made from the name and the draft's ignored. Refused with 400 (a
  /// rule broken), 507 (at max_schedules), 409 (name taken, every id the name gives taken by a
  /// file, or enabled on a target another enabled schedule drives: `holder` names it), 413 or
  /// 500 (not written). One whose target or preset is not on the device is saved, with a
  /// `warning`. Enabled, it applies the value in force at once.
  Result create(ScheduleConfig draft);
  /// Replaces a schedule's document under its id: 404, 409 for a file a newer firmware wrote,
  /// otherwise as create(). Enabled, it applies the value in force again, a rename too; a target
  /// it left is left as it is. `revision`, when given, is the one the caller read: one that is
  /// not the stored one is 409, since the Save would undo a change made since.
  Result update(const std::string &id, ScheduleConfig doc, optional<uint32_t> revision = nullopt);
  /// Brings a schedule back under the id the document names, enabled or not, as a backup holds
  /// it: one with that id is replaced as update() replaces it, otherwise one is created. Refused
  /// as those two refuse, and with 400 for an id that is no slug or is `new`, 409 for an id a
  /// file the boot did not load holds. The doc's revision is ignored: a replacement moves the
  /// stored one on, a new one starts at 0.
  Result restore(ScheduleConfig doc);
  /// Deletes a schedule; its target stays as it is.
  Result remove(const std::string &id);
  /// Stores the flag. Enabling one whose target another enabled schedule drives is 409, unless
  /// `take_over`: the others are stored disabled and named in `stopped`. A schedule that starts
  /// to drive its target applies the value in force; disabling leaves the target as it is. Each
  /// flag that changes moves its schedule's revision on.
  Result set_enabled(const std::string &id, bool enabled, bool take_over = false);

  /// Whether another schedule has this name, by name_key(); `exclude_id` never blocks itself.
  bool is_name_taken(const std::string &name, const std::string &exclude_id, std::string *error) const;

 protected:
  struct Entry {
    ScheduleConfig config;
    switch_::Switch *relay{nullptr};
    /// MISSING_TARGET or MISSING_PRESET as last resolved, RUNNING when all is there.
    ScheduleState missing{ScheduleState::RUNNING};
    std::string missing_reason;
    /// Enabled, all there, and the first enabled by id on its target: it drives it.
    bool active{false};
  };
  // The last tick's clock: local seconds, the loop's own count, and the minute seen last.
  struct ClockMark {
    int64_t local_s{0};
    uint64_t mono_ms{0};
    int64_t minute{0};
  };

  bool load_();
  bool load_file_(const std::string &folder, const std::string &filename);
  void resolve_name_(ScheduleConfig *config);

  Result add_(ScheduleConfig doc, const char *verb);
  Result replace_(Entry *stored, ScheduleConfig doc, const char *verb);
  /// After a change: who drives what is worked out again. `id` applies the value in force when
  /// `save` (a Save of it, enabled) or when it starts to drive its target; every other schedule
  /// the change let drive does too and goes into `started`.
  void settle_(const std::string &id, bool save, Result *result);
  /// Which entries drive their target now; returns the ids of those that did not before.
  std::vector<std::string> refresh_();
  /// Whether the target and every preset `config` names are on the device; the reason when not.
  ScheduleState check_target_(const ScheduleConfig &config, std::string *reason, switch_::Switch **relay) const;
  void resolve_(Entry &entry);
  /// The first enabled entry by id that names `config`'s target.
  const Entry *winner_(const ScheduleConfig &config) const;
  /// 409 for `doc`, enabled, on a target another enabled schedule drives.
  bool check_target_free_(const ScheduleConfig &doc, const std::string &self_id, Result *result) const;
  Entry *find_(const std::string &id) const;
  bool refuse_if_failed_(Result *result) const;

  void tick_();
  void start_all_(const char *why);
  /// Applies the value in force, if the entry drives its target and the clock has been valid.
  void start_(const std::string &id, const char *why);
  void apply_(const std::string &id, const Point &point, const char *why);
  void apply_relay_(const Entry &entry, const Point &point, const char *why);
  void apply_thermostat_(const Entry &entry, const Point &point, const char *why);
  void on_climate_change_(const std::string &climate_id);
  void on_released_(switch_::Switch *sw);

  // Seams: tests set the clocks and run what waits for the loop themselves.
  virtual ESPTime clock_now_();
  virtual uint64_t mono_ms_();
  virtual void later_(std::function<void()> &&job) { this->defer(std::move(job)); }

  std::string next_id_(const std::string &name) const;
  std::string folder_() const;
  std::string file_path_(const std::string &id) const;
  bool ensure_folder_();
  /// False, nothing written, when the write failed or a newer firmware wrote the file;
  /// `too_large` then says whether it was the size.
  bool save_(const ScheduleConfig &config, bool *too_large = nullptr);
  // Seam: every valid document fits the cap, and no host fails one allocation.
  virtual EncodeError encode_(const ScheduleConfig &config, std::string *json) const { return config.encode(json); }
  /// False when the file is still there to be loaded at the next boot.
  bool delete_file_(const std::string &id);
  // Seam: no host filesystem lets a test refuse one unlink.
  virtual bool remove_file_(const std::string &path);

  filesystem_storage_abstract::FilesystemStorageAbstract *storage_{nullptr};
  time::RealTimeClock *rtc_{nullptr};
  std::string folder_path_{"schedules"};
  uint8_t max_schedules_{16};

  // Sorted by id; unique_ptr so a job that looked one up keeps it while others come and go.
  std::vector<std::unique_ptr<Entry>> entries_;
  bool clock_valid_{false};
  ClockMark mark_;
  loop_job::LoopDispatcher dispatcher_;
};

}  // namespace schedules
}  // namespace esphome
