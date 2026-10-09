/*
 * @Description: Detects a device that keeps restarting itself shortly after boot
 * @License: GPL 3.0
 */
#include "app/restart_loop_guard.h"

#include <cstdint>

#include "esp_attr.h"
#include "esp_system.h"
#include "esp_timer.h"

namespace lilygo_box::app {
namespace {

constexpr uint32_t kMagic = 0x53564C47;
// Two quick restarts before this boot make it the third in a row.
constexpr uint32_t kLoopThreshold = 2;
constexpr uint64_t kStableUptimeUs = 60ULL * 1000 * 1000;

// Not initialised on a software reset, so both survive it; after a power
// cycle they hold noise, which the magic value tells apart.
RTC_NOINIT_ATTR uint32_t g_magic;
RTC_NOINIT_ATTR uint32_t g_quick_restarts;

void StableUptimeTimerCallback(void* argument) {
  (void)argument;
  g_quick_restarts = 0;
}

}  // namespace

bool IsDeviceInRestartLoop() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const bool restarted_itself =
      reason == ESP_RST_SW || reason == ESP_RST_PANIC ||
      reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
      reason == ESP_RST_WDT;
  if (g_magic != kMagic || !restarted_itself) {
    g_magic = kMagic;
    g_quick_restarts = 0;
  } else {
    ++g_quick_restarts;
  }

  // Staying up for a minute ends the streak. If the timer cannot be created
  // the count simply keeps growing, which only makes the guard more cautious.
  static esp_timer_handle_t timer = nullptr;
  if (timer == nullptr) {
    const esp_timer_create_args_t arguments = {
        .callback = StableUptimeTimerCallback,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "restart_guard",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&arguments, &timer) == ESP_OK) {
      esp_timer_start_once(timer, kStableUptimeUs);
    }
  }
  return g_quick_restarts >= kLoopThreshold;
}

}  // namespace lilygo_box::app
