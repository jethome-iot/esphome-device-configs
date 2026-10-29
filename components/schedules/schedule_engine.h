#pragma once

// The API an HTTP layer uses. Everything runs on the loop task: a caller on another task wraps
// its whole read, decide and write in one run_on_loop(job), as for climate_hub.
//
//   run_on_loop(job), is_failed(), max_schedules()
//   Reads, for the list and the editor:
//     schedules(), get(id)            the documents, sorted by id; document(id, doc) as the JSON a Save takes
//     not_loaded()                    how many files the boot left on flash for lack of memory
//     has_heap_room(more)             whether a change may take `more` PSRAM first and keep the reserves
//     target_name(target)             a relay's or a thermostat's name, its id when it is not there
//     status(id)                      ScheduleStatus: state, reason, note, and the issues of its actions
//     now_and_next(config, &a, &b)    the points in force and next, false while the clock is not valid;
//                                     a PointRef's point names its mode by index in config.modes
//     clock_text(&day, &time)         the device's local "wed", "10:15:42", false while not valid
//     relay_targets(), thermostat_targets()   what an action may drive, who holds and who schedules it
//     scheduled_by(type, id)          the enabled schedule that drives a relay or a thermostat
//   Writes, each a Result:
//     create(draft), update(id, doc, revision), restore(doc), remove(id), set_enabled(id, enabled, take_over)

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "esphome/components/filesystem_storage_abstract/filesystem_storage_abstract.h"
#include "esphome/components/loop_job/loop_job.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/optional.h"
#include "esphome/core/time.h"
#include "schedule_config.h"
#include "schedule_memory.h"

namespace esphome {

namespace time {
class RealTimeClock;
}
// A config without a switch: section has no switch sources; the name must still exist.
namespace switch_ {
class Switch;
}

namespace schedules {

// The schedules and their documents are PSRAM first (schedule_memory.h). Without PSRAM the
// HEAP_* limits guard the internal heap, which holds them; with it they live in PSRAM, which
// PSRAM_RESERVE_* guards, and the internal heap keeps PSRAM_HEAP_KEEP_* only. The internal heap
// is the part malloc can use: QEMU's debug sensor, which the figures below come from, also
// counts about 30 KiB of IRAM only 32-bit loads can use, so its numbers are that much higher.

/// Internal heap the boot keeps after each schedule it loads, without PSRAM. The services that
/// start after it take about 56 KiB for good, and the update check's TLS session about 50 KiB
/// more for a while (QEMU, Ethernet): under this the device runs out during that check.
static constexpr size_t BOOT_HEAP_FLOOR_BYTES = 130 * 1024;
static constexpr size_t BOOT_HEAP_FLOOR_BLOCK = 40 * 1024;
/// Internal heap a Save or an import leaves once done, without PSRAM: a little over the 74 KiB a
/// boot at its floor leaves once its services run, so what a Save keeps the next boot loads.
static constexpr size_t HEAP_KEEP_BYTES = 76 * 1024;
/// Internal heap a Save or an import leaves at its peak, without PSRAM, so the network stack and
/// the web server keep working through it.
static constexpr size_t HEAP_RESERVE_BYTES = 10 * 1024;
static constexpr size_t HEAP_RESERVE_BLOCK = 24 * 1024;
/// Internal heap a boot, a change and its peak leave with PSRAM. A schedule keeps about 90 B
/// there and a request works with about 6 KB (QEMU): room for 32 schedules and a request, so
/// their share never takes what the network stack runs on.
static constexpr size_t PSRAM_HEAP_KEEP_BYTES = 16 * 1024;
static constexpr size_t PSRAM_HEAP_KEEP_BLOCK = 4 * 1024;
/// PSRAM a boot, a Save or an import leaves to the rest of the firmware, which holds about 10 KB
/// of it for good (QEMU) and at times one block of up to 16 KiB, the update check's manifest.
static constexpr size_t PSRAM_RESERVE_BYTES = 64 * 1024;
static constexpr size_t PSRAM_RESERVE_BLOCK = 16 * 1024;
/// A body's parsed document takes up to about twice the body (the 16 KB worst case about its
/// size; host: 2.2 times, its slots twice as large), and a small one, a dense week of switch
/// points the densest, ArduinoJson's first pools and its strings.
static constexpr size_t PARSE_HEAP_FACTOR = 2;
static constexpr size_t PARSE_HEAP_MIN_BYTES = 8 * 1024;

/// What a body of `bytes` may take as a document, and the room checked for before its parse.
constexpr size_t parse_room(size_t bytes) {
  // ArduinoJson's slots hold a pointer: twice the size on a 64-bit host.
  return (PARSE_HEAP_FACTOR * bytes > PARSE_HEAP_MIN_BYTES ? PARSE_HEAP_FACTOR * bytes : PARSE_HEAP_MIN_BYTES) *
         (sizeof(void *) / 4);
}

/// A Save or an import the heap cannot take: 507.
extern const char *const NO_MEMORY_TO_SAVE;

/// One heap: what is free, and the largest block of it.
struct HeapRoom {
  size_t free{0};
  size_t largest_block{0};
};

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
  /// Saved enabled with an action that names what the device does not have, the first one:
  /// `skips mode "Day", action 2: relay "relay_7" is not on this device`.
  std::string warning;
  /// False when the change is live but did not reach flash, so a reboot undoes it.
  bool persisted{true};
  /// remove(): the schedule was a file the boot left on flash for lack of memory, never loaded.
  bool was_not_loaded{false};
  /// set_enabled() with take_over: the ids of the schedules it stored disabled, by id.
  std::vector<std::string> stopped;
  /// The ids of the schedules that started driving their targets because the change let them.
  std::vector<std::string> started;
};

enum class ScheduleState : uint8_t {
  DISABLED,
  WAITING_FOR_CLOCK,
  /// Enabled, but a schedule before it by id that runs drives one of its targets.
  CONFLICT,
  RUNNING,
};

const char *state_to_string(ScheduleState state);

/// Why an action cannot run as written right now.
enum class IssueKind : uint8_t { MISSING_RELAY, MISSING_THERMOSTAT, MISSING_PRESET, NO_THERMOSTATS, HELD };

const char *issue_kind_to_string(IssueKind kind);

struct Issue {
  /// Indexes in ScheduleConfig::modes and in that mode's actions.
  uint8_t mode{0};
  uint8_t action{0};
  IssueKind kind{IssueKind::MISSING_RELAY};
  /// `Relay "relay_7" is not on this device`, `Thermostat "Floor" drives Relay 3` and the like.
  std::string text;
  /// HELD: the name of the thermostat holding the relay.
  std::string held_by;
};

struct ScheduleStatus {
  ScheduleState state{ScheduleState::DISABLED};
  /// Why it does not run, "" when it runs or is disabled.
  std::string reason;
  /// No fault, but worth saying: a thermostat it drives is stopped.
  std::string note;
  /// Every action, in every mode, that the device would skip now, whatever the state.
  std::vector<Issue> issues;
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

/// Weekly schedules stored as <storage>/<folder>/<id>.json. Each one names modes, lists of
/// actions on relays and climate_hub thermostats with waits between them; at every switch
/// point its mode's list runs, and whenever the schedule starts the list of the mode in force
/// runs again.
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
  /// The files the boot left on flash for lack of memory and remove() has not deleted since.
  size_t not_loaded() const { return this->left_for_heap_.size(); }
  /// The documents, sorted by id.
  std::vector<const ScheduleConfig *> schedules() const;
  const ScheduleConfig *get(std::string_view id) const;
  /// The document as its file holds it, into `doc`; false for an unknown id. Whether it lost
  /// members to a failed allocation is doc.overflowed().
  bool document(const std::string &id, JsonDocument &doc) const;
  /// Whether the heaps keep their reserves now with `more` bytes taken PSRAM first, as a
  /// document takes them: a Save checks it before its parse and at its peak.
  bool has_heap_room(size_t more = 0) const;
  /// Where the schedule stands, checked now.
  ScheduleStatus status(std::string_view id) const;
  /// The relay's or the thermostat's name; its id when it is not on the device.
  std::string target_name(const Target &target) const;
  /// The point in force and the next one, the next even with the same mode. False while the
  /// clock has not been valid yet.
  bool now_and_next(const ScheduleConfig &config, PointRef *now, PointRef *next) const;
  /// The device's local day ("wed") and time ("10:15:42"); false while the clock is not valid.
  bool clock_text(std::string *day, std::string *time);
  /// Every visible switch.
  std::vector<RelayTarget> relay_targets() const;
  /// Every thermostat, none without climate_hub.
  std::vector<ThermostatTarget> thermostat_targets() const;
  /// The id of the schedule that runs and names the target; "" for none.
  std::string scheduled_by(TargetType type, const std::string &id) const;

  /// Adds a schedule; its id is made from the name and the draft's ignored. Refused with 400 (a
  /// rule broken), 507 (at max_schedules, files the boot left on flash for lack of memory, or
  /// the heaps would keep less than they must once it is held or on the way), 409 (name taken,
  /// every id the name gives taken by a file, or enabled on a target another enabled schedule
  /// drives: `holder` names it), 413 or 500 (not written). One whose actions name what the
  /// device does not have is saved, with a `warning`. Enabled, it runs the list of the mode in
  /// force at once.
  Result create(ScheduleConfig draft);
  /// Replaces a schedule's document under its id: 404, 409 for a file a newer firmware wrote,
  /// otherwise as create(), but one that holds no more than the stored one is never refused for
  /// room. Its list stops; enabled, the list of the mode in force runs again, a rename too. A
  /// target it left is left as it is. `revision`, when given, is the one the caller read: one
  /// that is not the stored one is 409, since the Save would undo a change made since.
  Result update(const std::string &id, ScheduleConfig doc, optional<uint32_t> revision = nullopt);
  /// Brings a schedule back under the id the document names, enabled or not, as a backup holds
  /// it: one with that id is replaced as update() replaces it, otherwise one is created. Refused
  /// as those two refuse, and with 400 for an id that is no slug or is `new`, 409 for an id a
  /// file the boot did not load holds. The doc's revision is ignored: a replacement moves the
  /// stored one on, a new one starts at 0.
  Result restore(ScheduleConfig doc);
  /// Deletes a schedule; its list stops and its targets stay as they are. A file the boot left
  /// on flash for lack of memory is deleted too, `was_not_loaded`, or 500 when it stays.
  Result remove(const std::string &id);
  /// Stores the flag. Enabling one that shares a target with another enabled schedule is 409,
  /// unless `take_over`: every other enabled one sharing a target is stored disabled and named
  /// in `stopped`. A schedule that starts to drive its targets runs the list of the mode in
  /// force; disabling stops its list and leaves the targets as they are. Each flag that changes
  /// moves its schedule's revision on.
  Result set_enabled(const std::string &id, bool enabled, bool take_over = false);

  /// Whether another schedule has this name, by name_key(); `exclude_id` never blocks itself.
  bool is_name_taken(std::string_view name, std::string_view exclude_id, std::string *error) const;

 protected:
  struct Entry {
    // PSRAM first, as the tables it holds.
    static void *operator new(size_t size) { return PsramAllocator<uint8_t>().allocate(size); }
    static void operator delete(void *ptr, size_t size) {
      PsramAllocator<uint8_t>().deallocate(static_cast<uint8_t *>(ptr), size);
    }

    ScheduleConfig config;
    /// Per target, the switch a relay target names; nullptr for a thermostat or a relay not on
    /// the device. Relays never come or go after the boot.
    PsramVector<switch_::Switch *> switches;
    /// Enabled, and no target of it is driven by one before it by id: it drives them all, the
    /// ones not on the device too. One that waits drives none of its targets.
    bool active{false};
    /// Its scheduler id, for the wait of its list.
    uint32_t timer{0};
    /// The list in force: its mode, -1 for none; the next action, its count once it is done.
    int8_t mode{-1};
    uint8_t next{0};
    bool waiting{false};
    /// A new value from next_run_ whenever a list starts or stops, so a step of an older one, or
    /// of a schedule deleted under the same id, goes no further.
    uint32_t run{0};
    /// The thermostat actions the list passed, by index, that did not reach their thermostat:
    /// once one of them can, the actions on that thermostat apply again.
    PsramVector<uint8_t> lost;
  };
  // The last tick's clock: local seconds, the loop's own count, and the minute seen last.
  struct ClockMark {
    int64_t local_s{0};
    uint64_t mono_ms{0};
    int64_t minute{0};
  };

  enum class Load : uint8_t { LOADED, REFUSED, NO_MEMORY };
  bool load_();
  /// NO_MEMORY, nothing kept, when the heaps are or would be under BOOT_HEAP_FLOOR_* or
  /// PSRAM_RESERVE_*.
  Load load_file_(const std::string &folder, const std::string &filename);
  bool boot_room_() const;
  void resolve_name_(ScheduleConfig *config);

  Result add_(ScheduleConfig doc, const char *verb);
  /// remove() of an id no schedule has: 404, unless the boot left its file on flash.
  Result remove_not_loaded_(const std::string &id);
  Result replace_(Entry *stored, ScheduleConfig doc, const char *verb);
  /// After a change: who drives what is worked out again. `id` runs its mode's list when `save`
  /// (a Save of it, enabled) or when it starts to drive its targets; every other schedule the
  /// change let drive does too and goes into `started`.
  void settle_(const std::string &id, bool save, Result *result);
  /// Which entries drive their targets now; stops the lists of those that no longer do, and
  /// returns the ids of those that did not before.
  std::vector<std::string> refresh_();
  /// The switches the entry's relay targets name.
  void resolve_(Entry &entry);
  /// The running entry that names `target`, nullptr for none.
  const Entry *holder_(const Target &target) const;
  /// 409 for `doc`, enabled, on a target another running schedule drives: the first such target.
  bool check_target_free_(const ScheduleConfig &doc, std::string_view self_id, Result *result) const;
  /// The actions of `config` the device would skip now; `switches` as Entry's.
  std::vector<Issue> issues_(const ScheduleConfig &config, const PsramVector<switch_::Switch *> &switches) const;
  Entry *find_(std::string_view id) const;
  bool refuse_if_failed_(Result *result) const;
  /// 507 for a schedule under a new id while files wait on flash: the next boot could load it
  /// in place of one of them, which the list never showed.
  bool refuse_if_files_wait_(Result *result) const;
  /// Whether the heaps keep what they must once `doc` is held and `replaced`, if any, is gone:
  /// HEAP_KEEP_BYTES without PSRAM, PSRAM_HEAP_KEEP_BYTES and PSRAM_RESERVE_BYTES with it.
  bool keeps_room_(const ScheduleConfig &doc, const ScheduleConfig *replaced) const;
  /// "160000 bytes free, 90000 in one block", and PSRAM's after it, for the log.
  std::string room_text_() const;

  void tick_();
  void start_all_(const char *why);
  /// Runs the list of the mode in force, if the entry drives its targets and the clock has been valid.
  void start_(const std::string &id, const char *why);
  /// Stops whatever list ran and runs `mode`'s from its first action.
  void run_(const std::string &id, uint8_t mode, const char *why);
  /// Applies the list's actions up to its next wait, or to its end.
  void advance_(std::string id);
  /// Stops the entry's list where it is.
  void cancel_(Entry &entry);
  /// Applies a RELAY or THERMOSTAT action, `index` in the list; one whose target is not there is
  /// skipped. A switch's callbacks run last and may change anything, `entry` included.
  void apply_relay_(Entry &entry, const Action &action);
  void apply_thermostat_(Entry &entry, const Action &action, uint8_t index);
  /// Remembers that the list's action at `index` did not reach its thermostat.
  void mark_lost_(const std::string &id, uint32_t run, uint8_t index);
  /// Whether the hub has the thermostat, and the preset when `key` names one.
  bool thermostat_ready_(const std::string &thermostat, const std::string &key) const;
  void on_climate_change_(const std::string &climate_id);
  /// The thermostat `target` names took back what it lost: the actions on it the list passed
  /// apply again, in order; one that still cannot is skipped and stays lost.
  void reapply_(const std::string &id, uint32_t run, uint8_t target);
  void on_released_(switch_::Switch *sw);
  /// The relay `target` names is free: it takes the last action on it the list passed.
  void follow_relay_(const std::string &id, uint32_t run, uint8_t target);

  // Seams: tests set the clocks and run what waits for the loop and a wait's end themselves.
  virtual ESPTime clock_now_();
  virtual uint64_t mono_ms_();
  virtual void later_(std::function<void()> &&job) { this->defer(std::move(job)); }
  virtual void wait_(uint32_t timer, uint32_t ms, std::function<void()> &&job) {
    this->set_timeout(timer, ms, std::move(job));
  }
  virtual void cancel_wait_(uint32_t timer) { this->cancel_timeout(timer); }

  std::string next_id_(std::string_view name) const;
  std::string folder_() const;
  std::string file_path_(std::string_view id) const;
  bool ensure_folder_();
  /// False, nothing written, when the write failed or a newer firmware wrote the file; `why`
  /// then says whether it was the size or the heap. `grows`: a change that keeps more in RAM,
  /// refused as NO_MEMORY when its peak leaves a heap under its reserve.
  bool save_(const ScheduleConfig &config, EncodeError *why = nullptr, bool grows = false);
  // Seams: every valid document fits the cap, no host heap runs out, and a host has no PSRAM.
  virtual size_t max_file_bytes_() const { return MAX_FILE_BYTES; }
  virtual ArduinoJson::Allocator *json_allocator_() const { return json_allocator(); }
  /// The internal heap malloc can use.
  virtual HeapRoom heap_() const;
  /// PSRAM; none on a board without it.
  virtual optional<HeapRoom> psram_() const;
  /// False when the file is still there to be loaded at the next boot.
  bool delete_file_(const std::string &id);
  // Seam: no host filesystem lets a test refuse one unlink.
  virtual bool remove_file_(const std::string &path);

  filesystem_storage_abstract::FilesystemStorageAbstract *storage_{nullptr};
  time::RealTimeClock *rtc_{nullptr};
  std::string folder_path_{"schedules"};
  uint8_t max_schedules_{16};
  // The names of the files the boot left on flash for lack of heap, sorted.
  std::vector<std::string> left_for_heap_;

  // Sorted by id; unique_ptr so a job that looked one up keeps it while others come and go.
  std::vector<std::unique_ptr<Entry>> entries_;
  // Never reused, so a wait never answers to another schedule's id.
  uint32_t next_timer_{1};
  // Never reused either: a job queued for a list goes no further once another list started.
  uint32_t next_run_{1};
  bool clock_valid_{false};
  ClockMark mark_;
  loop_job::LoopDispatcher dispatcher_;
};

}  // namespace schedules
}  // namespace esphome
