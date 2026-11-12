#include "gain_sets.h"
#include <algorithm>
#include <cinttypes>
#include <cstring>

namespace esphome::climate_hub {

namespace {

bool fail(std::string *error, const char *message) {
  if (error != nullptr)
    *error = message;
  return false;
}

// The length of the well-formed UTF-8 sequence at `i`, 0 for none, and its code point: no
// overlong form, no surrogate, nothing past U+10FFFF.
size_t utf8_at(const std::string &text, size_t i, uint32_t *code_point) {
  const auto lead = static_cast<uint8_t>(text[i]);
  if (lead < 0x80) {
    *code_point = lead;
    return 1;
  }
  if (lead < 0xC2 || lead > 0xF4)
    return 0;
  const size_t length = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : 2;
  uint8_t low = 0x80;
  uint8_t high = 0xBF;
  if (lead == 0xE0)
    low = 0xA0;
  if (lead == 0xED)
    high = 0x9F;
  if (lead == 0xF0)
    low = 0x90;
  if (lead == 0xF4)
    high = 0x8F;
  if (i + length > text.size())
    return 0;
  uint32_t value = lead & (0x7F >> length);
  for (size_t k = 1; k < length; k++) {
    const auto next = static_cast<uint8_t>(text[i + k]);
    if (next < (k == 1 ? low : 0x80) || next > (k == 1 ? high : 0xBF))
      return 0;
    value = (value << 6) | (next & 0x3F);
  }
  *code_point = value;
  return length;
}

bool is_control(uint32_t code_point) { return code_point < 0x20 || (code_point >= 0x7F && code_point <= 0x9F); }

void set_or_null(JsonObject obj, const char *key, float value) {
  if (std::isfinite(value)) {
    obj[key] = value;
  } else {
    obj[key] = nullptr;
  }
}

// Gains that cannot be applied make no set: a NaN would poison the PID.
bool read_gain(JsonVariantConst value, float *out) {
  if (!value.is<float>())
    return false;
  *out = value.as<float>();
  return std::isfinite(*out);
}

float read_measurement(JsonVariantConst value) {
  if (!value.is<float>())
    return NAN;
  const float out = value.as<float>();
  return std::isfinite(out) ? out : NAN;
}

// A line's own `n`, and whether that is all it holds: what a removal leaves behind for the number.
struct LineNumber {
  uint32_t n{0};
  bool keeper{false};
};

LineNumber number_of(JsonObjectConst obj) {
  LineNumber out;
  JsonVariantConst n = obj["n"];
  if (n.is<uint32_t>() && n.as<uint32_t>() > 0) {
    out.n = n.as<uint32_t>();
    out.keeper = obj.size() == 1;
  }
  return out;
}

// One object per line, nothing nested deeper than its flags: the listing parses on the web
// server's small stack.
bool parse_line(const std::string &line, JsonDocument *doc) {
  return !deserializeJson(*doc, line, DeserializationOption::NestingLimit(3)) && doc->is<JsonObject>();
}

}  // namespace

bool GainSet::has_flag(const char *flag) const {
  return std::find(this->flags.begin(), this->flags.end(), flag) != this->flags.end();
}

bool validate_set_name(const std::string &name, std::string *error) {
  if (name.empty())
    return fail(error, "Name is required");
  size_t chars = 0;
  bool control = false;
  for (size_t i = 0; i < name.size(); chars++) {
    uint32_t code_point;
    const size_t length = utf8_at(name, i, &code_point);
    if (length == 0)
      return fail(error, "Name is not valid UTF-8");
    control = control || is_control(code_point);
    i += length;
  }
  static_assert(GAIN_SET_NAME_MAX_LENGTH == 48, "the sentence names the limit");
  if (chars > GAIN_SET_NAME_MAX_LENGTH)
    return fail(error, "Name is longer than 48 characters");
  if (control)
    return fail(error, "Name cannot contain control characters");
  return true;
}

const char *rule_label(AutotuneRule rule) {
  switch (rule) {
    case AutotuneRule::ZN_PI:
      return "ZN PI";
    case AutotuneRule::ZN_PID:
      return "ZN PID";
    case AutotuneRule::PESSEN:
      return "Pessen";
    case AutotuneRule::SOME_OVERSHOOT:
      return "Some overshoot";
    case AutotuneRule::NO_OVERSHOOT:
      return "No overshoot";
  }
  return "ZN PI";
}

std::string default_set_name(const ESPTime &now, Direction direction, const char *label, uint32_t n) {
  char name[64];
  if (now.is_valid()) {
    snprintf(name, sizeof(name), "%04u-%02u-%02u %02u:%02u %s", static_cast<unsigned>(now.year) % 10000u,
             now.month % 100u, now.day_of_month % 100u, now.hour % 100u, now.minute % 100u, label);
  } else {
    snprintf(name, sizeof(name), "%s %s #%" PRIu32, direction == Direction::COOL ? "Cool" : "Heat", label, n);
  }
  return name;
}

void serialize_gain_set(JsonObject obj, const GainSet &set) {
  obj["n"] = set.n;
  obj["name"] = set.name;
  obj["direction"] = enums::direction_to_string(set.direction);
  obj["source"] = set.source;
  if (set.rule.empty()) {
    obj["rule"] = nullptr;
  } else {
    obj["rule"] = set.rule;
  }
  set_or_null(obj, "ku", set.ku);
  set_or_null(obj, "pu", set.pu);
  obj["kp"] = set.gains.kp;
  obj["ki"] = set.gains.ki;
  obj["kd"] = set.gains.kd;
  JsonArray flags = obj["flags"].to<JsonArray>();
  for (const std::string &flag : set.flags)
    flags.add(flag);
  if (set.time.has_value()) {
    obj["time"] = *set.time;
  } else {
    obj["time"] = nullptr;
  }
  if (set.from.has_value()) {
    obj["from"] = *set.from;
  } else {
    obj["from"] = nullptr;
  }
}

bool encode_gain_set(const GainSet &set, std::string *line) {
  JsonDocument doc;
  serialize_gain_set(doc.to<JsonObject>(), set);
  std::string out;
  serializeJson(doc, out);
  if (doc.overflowed() || out.size() > GAIN_SET_LINE_MAX_BYTES)
    return false;
  *line = std::move(out);
  return true;
}

bool decode_gain_set(JsonObjectConst obj, GainSet *out) {
  GainSet set;
  const LineNumber number = number_of(obj);
  if (number.n == 0 || number.keeper)
    return false;
  set.n = number.n;
  if (!obj["name"].is<const char *>() || !obj["direction"].is<const char *>())
    return false;
  set.name = obj["name"].as<std::string>();
  if (!enums::direction_from_string(obj["direction"].as<std::string>(), &set.direction))
    return false;
  if (!read_gain(obj["kp"], &set.gains.kp) || !read_gain(obj["ki"], &set.gains.ki) ||
      !read_gain(obj["kd"], &set.gains.kd))
    return false;
  if (obj["source"].is<const char *>())
    set.source = obj["source"].as<std::string>();
  if (obj["rule"].is<const char *>())
    set.rule = obj["rule"].as<std::string>();
  set.ku = read_measurement(obj["ku"]);
  set.pu = read_measurement(obj["pu"]);
  for (JsonVariantConst flag : obj["flags"].as<JsonArrayConst>()) {
    if (flag.is<const char *>())
      set.flags.push_back(flag.as<std::string>());
  }
  if (obj["time"].is<int64_t>())
    set.time = obj["time"].as<int64_t>();
  if (obj["from"].is<uint32_t>())
    set.from = obj["from"].as<uint32_t>();
  *out = std::move(set);
  return true;
}

bool decode_gain_set(const std::string &line, GainSet *out) {
  JsonDocument doc;
  return parse_line(line, &doc) && decode_gain_set(doc.as<JsonObjectConst>(), out);
}

// --- LineReader ---

bool LineReader::fill_() {
  if (this->eof_ || this->file_ == nullptr)
    return false;
  this->pos_ = 0;
  this->end_ = fread(this->chunk_, 1, sizeof(this->chunk_), this->file_);
  if (this->end_ == 0) {
    this->eof_ = true;
    this->failed_ = ferror(this->file_) != 0;
    return false;
  }
  return true;
}

int LineReader::peek_() {
  if (this->pos_ == this->end_ && !this->fill_())
    return EOF;
  return static_cast<uint8_t>(this->chunk_[this->pos_]);
}

int LineReader::get_() {
  const int c = this->peek_();
  if (c != EOF)
    this->pos_++;
  return c;
}

bool LineReader::next(std::string *line, bool *whole) {
  if (this->cut_)
    this->rest(nullptr);
  line->clear();
  if (this->peek_() == EOF)
    return false;
  for (;;) {
    const int c = this->get_();
    // The last line may have no newline: a power cut in the middle of an append.
    if (c == EOF || c == '\n') {
      *whole = true;
      return true;
    }
    line->push_back(static_cast<char>(c));
    if (line->size() < this->cap_)
      continue;
    const int after = this->peek_();
    if (after == '\n')
      this->get_();
    *whole = after == '\n' || after == EOF;
    this->cut_ = !*whole;
    return true;
  }
}

bool LineReader::rest(const std::function<bool(const char *, size_t)> &sink) {
  if (!this->cut_)
    return true;
  this->cut_ = false;
  bool ok = true;
  while (this->pos_ < this->end_ || this->fill_()) {
    const char *start = this->chunk_ + this->pos_;
    const auto *newline = static_cast<const char *>(memchr(start, '\n', this->end_ - this->pos_));
    const size_t len = newline != nullptr ? static_cast<size_t>(newline - start) : this->end_ - this->pos_;
    if (ok && sink)
      ok = sink(start, len);
    this->pos_ += len;
    if (newline != nullptr) {
      this->pos_++;
      break;
    }
  }
  return ok;
}

// --- The file ---

uint32_t max_gain_set_n(const std::string &path) {
  FILE *file = fopen(path.c_str(), "r");
  if (file == nullptr)
    return 0;
  LineReader lines(file, GAIN_SET_LINE_MAX_BYTES);
  std::string line;
  bool whole;
  uint32_t max = 0;
  while (lines.next(&line, &whole)) {
    JsonDocument doc;
    if (whole && parse_line(line, &doc))
      max = std::max(max, number_of(doc.as<JsonObjectConst>()).n);
  }
  fclose(file);
  return max;
}

bool find_gain_set(const std::string &path, uint32_t n, GainSet *out) {
  FILE *file = fopen(path.c_str(), "r");
  if (file == nullptr)
    return false;
  LineReader lines(file, GAIN_SET_LINE_MAX_BYTES);
  std::string line;
  bool whole;
  bool found = false;
  while (!found && lines.next(&line, &whole)) {
    GainSet set;
    if (whole && decode_gain_set(line, &set) && set.n == n) {
      *out = std::move(set);
      found = true;
    }
  }
  fclose(file);
  return found;
}

bool append_gain_set(const std::string &path, const GainSet &set) {
  std::string line;
  if (!encode_gain_set(set, &line))
    return false;
  // A line torn by a power cut stays a line of its own, which no reader takes for a set.
  if (FILE *in = fopen(path.c_str(), "r")) {
    if (fseek(in, -1, SEEK_END) == 0 && fgetc(in) != '\n')
      line.insert(0, "\n");
    fclose(in);
  }
  line.push_back('\n');
  FILE *out = fopen(path.c_str(), "a");
  if (out == nullptr)
    return false;
  const size_t written = fwrite(line.data(), 1, line.size(), out);
  // The close is where a full filesystem shows up.
  const bool closed = fclose(out) == 0;
  return written == line.size() && closed;
}

GainSetRewrite rewrite_gain_sets(const std::string &path, uint32_t n, const std::string *name, GainSet *changed) {
  FILE *in = fopen(path.c_str(), "r");
  if (in == nullptr)
    return GainSetRewrite::NOT_FOUND;
  const std::string tmp = path + ".tmp";
  FILE *out = fopen(tmp.c_str(), "w");
  if (out == nullptr) {
    fclose(in);
    return GainSetRewrite::FAILED;
  }
  bool ok = true;
  auto put = [&](const char *data, size_t len) {
    ok = ok && fwrite(data, 1, len, out) == len;
    return ok;
  };
  LineReader lines(in, GAIN_SET_LINE_MAX_BYTES);
  std::string line;
  bool whole;
  bool found = false;
  uint32_t max_seen = 0;
  uint32_t max_kept = 0;
  while (ok && lines.next(&line, &whole)) {
    if (whole && line.empty())
      continue;
    JsonDocument doc;
    LineNumber number;
    if (whole && parse_line(line, &doc))
      number = number_of(doc.as<JsonObjectConst>());
    max_seen = std::max(max_seen, number.n);
    // Written again at the end when it is still the highest: one such line at most.
    if (number.keeper)
      continue;
    GainSet set;
    if (!found && number.n == n && decode_gain_set(doc.as<JsonObjectConst>(), &set)) {
      found = true;
      if (name == nullptr) {
        if (changed != nullptr)
          *changed = std::move(set);
        continue;
      }
      // Its other keys as they were, a newer firmware's among them.
      doc["name"] = *name;
      line.clear();
      serializeJson(doc, line);
      set.name = *name;
      if (changed != nullptr)
        *changed = std::move(set);
    }
    put(line.data(), line.size());
    if (!whole)
      lines.rest(put);
    put("\n", 1);
    max_kept = std::max(max_kept, number.n);
  }
  ok = ok && !lines.failed();
  if (ok && found && max_seen > max_kept) {
    const std::string keeper = "{\"n\":" + std::to_string(max_seen) + "}\n";
    put(keeper.data(), keeper.size());
  }
  fclose(in);
  const bool closed = fclose(out) == 0;
  if (!found || !ok || !closed || rename(tmp.c_str(), path.c_str()) != 0) {
    ::remove(tmp.c_str());
    return found ? GainSetRewrite::FAILED : GainSetRewrite::NOT_FOUND;
  }
  return GainSetRewrite::DONE;
}

// --- GainSetReader ---

GainSetReader::GainSetReader(FILE *file, std::atomic<uint8_t> *open)
    : file_(file), lines_(file, GAIN_SET_LINE_MAX_BYTES), open_(open) {}

GainSetReader::~GainSetReader() {
  if (this->file_ != nullptr)
    fclose(this->file_);
  if (this->open_ != nullptr)
    this->open_->fetch_sub(1);
}

bool GainSetReader::next(GainSet *set) {
  bool whole;
  while (this->lines_.next(&this->line_, &whole)) {
    if (whole && decode_gain_set(this->line_, set))
      return true;
  }
  return false;
}

}  // namespace esphome::climate_hub
