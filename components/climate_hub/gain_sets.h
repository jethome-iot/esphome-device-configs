#pragma once

#include <ArduinoJson.h>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>
#include "enums.h"
#include "esphome/core/optional.h"
#include "esphome/core/time.h"
#include "pid_core.h"

namespace esphome::climate_hub {

/// The folder below the thermostats' that holds each one's sets as `<id>.jsonl`.
static constexpr const char *GAIN_SET_FOLDER = "gains";
/// Longest set name, in characters.
static constexpr size_t GAIN_SET_NAME_MAX_LENGTH = 48;
/// Longest line the device reads as a set; one it writes stays far under it.
static constexpr size_t GAIN_SET_LINE_MAX_BYTES = 1024;
/// A new set is refused with less than this free on the storage: the thermostats' own files
/// need the room more.
static constexpr size_t GAIN_SET_MIN_FREE_BYTES = 64 * 1024;
/// Most sets one listing answers with; the rest come with `after`.
static constexpr uint32_t GAIN_SET_PAGE_MAX = 200;

/// The words of a set's `source`.
static constexpr const char *GAIN_SET_CALIBRATION = "calibration";
static constexpr const char *GAIN_SET_DERIVED = "derived";
static constexpr const char *GAIN_SET_MANUAL = "manual";

/// One saved set of a relay's gains: a line of the thermostat's sets file, and a row of the API.
struct GainSet {
  /// From 1, never given twice in one file.
  uint32_t n{0};
  std::string name;
  Direction direction{Direction::HEAT};
  /// GAIN_SET_CALIBRATION, GAIN_SET_DERIVED or GAIN_SET_MANUAL; a newer firmware's word as it is.
  std::string source{GAIN_SET_MANUAL};
  /// The rule that made the gains, "" for none.
  std::string rule;
  /// What the calibration measured, NaN for none; Pu in seconds.
  float ku{NAN};
  float pu{NAN};
  PidGains gains;
  /// asymmetric, uneven, clamped: the calibration's warnings.
  std::vector<std::string> flags;
  /// When it was saved, UTC seconds, none while the device's clock was not set.
  optional<int64_t> time;
  /// The set a derived one was made from.
  optional<uint32_t> from;

  bool has_measurement() const { return std::isfinite(this->ku) && std::isfinite(this->pu); }
  bool has_flag(const char *flag) const;
};

/// The name rules on a trimmed set name: UTF-8 text of 1..GAIN_SET_NAME_MAX_LENGTH characters
/// without control characters. Two sets may share one.
bool validate_set_name(const std::string &name, std::string *error);

/// How a set's name says which rule made it.
const char *rule_label(AutotuneRule rule);

/// A set's name when it is given none: the local date and time it was saved, when the clock
/// is set, and `label`; otherwise its relay, `label` and its number.
std::string default_set_name(const ESPTime &now, Direction direction, const char *label, uint32_t n);

/// Its line's object, keys in a fixed order: the API answers with the same.
void serialize_gain_set(JsonObject obj, const GainSet &set);

/// Its line, without the newline; false when it would be over GAIN_SET_LINE_MAX_BYTES.
bool encode_gain_set(const GainSet &set, std::string *line);

/// A set from a line's object; false when the line is no set this firmware can apply.
bool decode_gain_set(JsonObjectConst obj, GainSet *out);
bool decode_gain_set(const std::string &line, GainSet *out);

/// A sets file's lines in constant RAM. A line past `cap` bytes, which only a hand edit makes,
/// comes cut, and the rest of it waits for rest().
class LineReader {
 public:
  LineReader(FILE *file, size_t cap) : file_(file), cap_(cap) {}

  /// The next line without its newline; false at the end of the file. `whole` is false for a cut
  /// one. The rest of a cut line nobody asked for is skipped.
  bool next(std::string *line, bool *whole);
  /// The rest of a cut line up to its newline, handed to `sink` in pieces, or skipped without one.
  bool rest(const std::function<bool(const char *, size_t)> &sink);
  /// The file could not be read to its end.
  bool failed() const { return this->failed_; }

 protected:
  bool fill_();
  int peek_();
  int get_();

  FILE *file_;
  size_t cap_;
  char chunk_[128];
  size_t pos_{0};
  size_t end_{0};
  bool eof_{false};
  bool failed_{false};
  bool cut_{false};
};

/// The highest `n` in the file, of a set or of any line that carries one, 0 for none: the next
/// set takes the one after.
uint32_t max_gain_set_n(const std::string &path);

/// The first set with `n`; false when there is none.
bool find_gain_set(const std::string &path, uint32_t n, GainSet *out);

/// Adds the set as the file's last line, after a newline when the file does not end in one.
bool append_gain_set(const std::string &path, const GainSet &set);

enum class GainSetRewrite : uint8_t { DONE, NOT_FOUND, FAILED };

/// Writes the file again beside itself and renames it over the old one, without the set `n`, or
/// with `name` as its name: every other line as it was, one that is no set too. A removed set
/// whose number was the highest leaves a line that keeps it. `changed` gets the set as it is now.
GainSetRewrite rewrite_gain_sets(const std::string &path, uint32_t n, const std::string *name, GainSet *changed);

/// A thermostat's sets, one at a time in file order, for a listing on the web server's task. The
/// hub counts the open ones in `open` and writes nothing to the file while there are any.
class GainSetReader {
 public:
  /// `file` is null when the thermostat has no sets yet.
  GainSetReader(FILE *file, std::atomic<uint8_t> *open);
  ~GainSetReader();
  GainSetReader(const GainSetReader &) = delete;
  GainSetReader &operator=(const GainSetReader &) = delete;

  /// The next line that is a set; false at the end.
  bool next(GainSet *set);

 protected:
  FILE *file_;
  LineReader lines_;
  std::atomic<uint8_t> *open_;
  std::string line_;
};

}  // namespace esphome::climate_hub
