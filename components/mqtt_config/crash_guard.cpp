#include "crash_guard.h"
#include "esphome/core/defines.h"

#ifdef USE_ESP32
#include <esp_attr.h>
#include <esp_system.h>
#endif

namespace esphome::mqtt_config {

uint8_t next_streak(const CrashGuardRecord &rec, bool magic_ok, bool panic_reset) {
  if (!magic_ok || !panic_reset || rec.armed == 0)
    return 0;
  return rec.streak == UINT8_MAX ? UINT8_MAX : static_cast<uint8_t>(rec.streak + 1);
}

#ifdef USE_ESP32
// Not initialised on boot, so a panic or a watchdog leaves it as it was; nothing touches flash.
static RTC_NOINIT_ATTR CrashGuardRecord rtc_record;

CrashGuardRecord &rtc_guard_record() { return rtc_record; }

bool reset_was_panic() {
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
      return true;
    default:
      return false;
  }
}
#else
CrashGuardRecord &rtc_guard_record() {
  static CrashGuardRecord record{};
  return record;
}

bool reset_was_panic() { return false; }
#endif

}  // namespace esphome::mqtt_config
