#pragma once

#include <cstdint>

namespace esphome::mqtt_config {

// A crash streak counted across resets in RTC memory: a large message on a subscribed topic
// can abort the device, and a retained one does it again on every connect. A connection that
// lasts DISARM_AFTER_MS proves the client harmless and resets the count.
struct CrashGuardRecord {
  uint32_t magic;
  uint8_t streak;  // crashes in a row while armed, as of this boot
  uint8_t armed;   // set from a connect until the connection has lasted DISARM_AFTER_MS
  uint8_t reserved[2];
};

static constexpr uint32_t CRASH_GUARD_MAGIC = 0x4D51544Eu;
static constexpr uint8_t SUSPEND_SUBSCRIPTIONS_AT = 2;
static constexpr uint8_t HOLD_MQTT_AT = 3;
static constexpr uint32_t DISARM_AFTER_MS = 60000;

// This boot's streak: one more after a panic or watchdog reset that came while armed, else 0.
// A record with a bad magic is power-on garbage and counts as 0.
uint8_t next_streak(const CrashGuardRecord &rec, bool magic_ok, bool panic_reset);

// The record in RTC memory that survives a panic, and whether this boot followed one. ESP32
// only; elsewhere a plain static and never a panic.
CrashGuardRecord &rtc_guard_record();
bool reset_was_panic();

}  // namespace esphome::mqtt_config
