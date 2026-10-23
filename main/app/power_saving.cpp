/*
 * @Description: Low Power Mode state and the limits it puts on the system
 * @License: GPL 3.0
 */
#include "app/power_saving.h"

#include <algorithm>
#include <mutex>
#include <utility>

#include "base/logger.h"
#include "nvs.h"

namespace lilygo_box::app {
namespace {

constexpr char kNvsNamespace[] = "svos";
constexpr char kNvsKey[] = "low_power";

constexpr uint8_t kEnabledBit = 1U << 0;
constexpr uint8_t kAutoEnableBit = 1U << 1;
constexpr uint8_t kAutomaticBit = 1U << 2;
constexpr uint8_t kRestoreWifiBit = 1U << 3;
// Set on every saved value, so that a missing key (a fresh install) and a
// saved "everything off" can be told apart.
constexpr uint8_t kStoredBit = 1U << 7;

std::mutex g_mutex;
LowPowerState g_state;
std::function<void(bool)> g_listener;
// The user switched the mode off while the battery was low. Without this the
// automatic switch would turn it straight back on at the next reading; it
// waits for a charge instead. Deliberately not saved: a reboot asks again.
bool g_auto_suppressed = false;
// Battery readings can arrive before the saved state is loaded and before
// anything listens; acting on them then would be overwritten or lost.
bool g_initialized = false;

uint8_t Encode(const LowPowerState& state) {
  return kStoredBit | (state.enabled ? kEnabledBit : 0) |
      (state.auto_enable ? kAutoEnableBit : 0) |
      (state.enabled_automatically ? kAutomaticBit : 0) |
      (state.restore_wifi ? kRestoreWifiBit : 0);
}

void Save(const LowPowerState& state) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Save Low Power Mode state failed\n");
    return;
  }
  nvs_set_u8(handle, kNvsKey, Encode(state));
  nvs_commit(handle);
  nvs_close(handle);
}

/**
 * @brief Stores a new state and tells the listener when the mode flipped.
 * @param change Edits the state; runs under the lock.
 */
template <typename Change>
void Modify(Change change) {
  bool mode_changed = false;
  bool enabled = false;
  LowPowerState saved;
  std::function<void(bool)> listener;
  {
    std::lock_guard<std::mutex> guard(g_mutex);
    const LowPowerState before = g_state;
    change(&g_state);
    if (Encode(before) == Encode(g_state)) {
      return;
    }
    mode_changed = before.enabled != g_state.enabled;
    enabled = g_state.enabled;
    saved = g_state;
    listener = g_listener;
  }
  Save(saved);
  if (mode_changed) {
    LogMessage(LogLevel::kInfo, __FILE__, __LINE__, "Low Power Mode %s%s\n",
        enabled ? "on" : "off",
        saved.enabled_automatically ? " (battery low)" : "");
    if (listener) {
      listener(enabled);
    }
  }
}

}  // namespace

void InitLowPowerStorage() {
  nvs_handle_t handle = 0;
  uint8_t stored = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
    if (nvs_get_u8(handle, kNvsKey, &stored) != ESP_OK) {
      stored = 0;
    }
    nvs_close(handle);
  }
  std::lock_guard<std::mutex> guard(g_mutex);
  g_initialized = true;
  if ((stored & kStoredBit) == 0) {
    g_state = LowPowerState();
    return;
  }
  g_state.enabled = (stored & kEnabledBit) != 0;
  g_state.auto_enable = (stored & kAutoEnableBit) != 0;
  g_state.enabled_automatically = (stored & kAutomaticBit) != 0;
  g_state.restore_wifi = (stored & kRestoreWifiBit) != 0;
}

LowPowerState GetLowPowerState() {
  std::lock_guard<std::mutex> guard(g_mutex);
  return g_state;
}

bool IsLowPowerModeEnabled() {
  std::lock_guard<std::mutex> guard(g_mutex);
  return g_state.enabled;
}

void SetLowPowerModeEnabled(bool enabled) {
  {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!enabled && g_state.enabled) {
      g_auto_suppressed = true;
    }
  }
  Modify([enabled](LowPowerState* state) {
    state->enabled = enabled;
    state->enabled_automatically = false;
  });
}

void SetLowPowerAutoEnable(bool enabled) {
  Modify([enabled](LowPowerState* state) { state->auto_enable = enabled; });
}

void SetLowPowerWifiRestore(bool restore) {
  Modify([restore](LowPowerState* state) { state->restore_wifi = restore; });
}

void UpdateLowPowerForBattery(int percent, bool charging) {
  bool turn_on = false;
  bool turn_off = false;
  {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_initialized || !g_listener) {
      return;
    }
    if (charging || percent > kLowPowerAutoEnablePercent) {
      g_auto_suppressed = false;
    }
    // 0 % is what an unread gauge reports, so it never triggers anything.
    turn_on = !g_state.enabled && g_state.auto_enable && !g_auto_suppressed &&
        !charging && percent > 0 && percent <= kLowPowerAutoEnablePercent;
    turn_off = g_state.enabled && g_state.enabled_automatically && charging &&
        percent >= kLowPowerAutoDisablePercent;
  }
  if (turn_on) {
    Modify([](LowPowerState* state) {
      state->enabled = true;
      state->enabled_automatically = true;
    });
  } else if (turn_off) {
    Modify([](LowPowerState* state) {
      state->enabled = false;
      state->enabled_automatically = false;
    });
  }
}

void SetLowPowerModeListener(std::function<void(bool enabled)> listener) {
  std::lock_guard<std::mutex> guard(g_mutex);
  g_listener = std::move(listener);
}

int LimitBrightnessForLowPower(int percent) {
  return IsLowPowerModeEnabled()
      ? std::min(percent, kLowPowerBrightnessCapPercent)
      : percent;
}

DisplayPreferences LimitDisplayPreferencesForLowPower(
    DisplayPreferences preferences) {
  if (!IsLowPowerModeEnabled()) {
    return preferences;
  }
  preferences.brightness_percent =
      std::min(preferences.brightness_percent, kLowPowerBrightnessCapPercent);
  if (preferences.lock_timeout_seconds == kDisplayLockTimeoutDisabledSeconds ||
      preferences.lock_timeout_seconds > kLowPowerLockTimeoutSeconds) {
    preferences.lock_timeout_seconds = kLowPowerLockTimeoutSeconds;
  }
  return preferences;
}

}  // namespace lilygo_box::app
