/*
 * @Description: Low Power Mode state and the limits it puts on the system
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>
#include <functional>

#include "app/storage/display_storage.h"

namespace lilygo_box::app {

// What Low Power Mode does. The AMOLED panel and the Wi-Fi chip are the two
// big consumers that are not doing the user's work, so those carry the cuts.
inline constexpr int kLowPowerBrightnessCapPercent = 30;
inline constexpr int kLowPowerLockTimeoutSeconds = 30;
// ~15 frames per second instead of 30, and touch read every 20 ms, not 10.
inline constexpr uint32_t kLowPowerRefreshPeriodMs = 66;
inline constexpr uint32_t kLowPowerTouchPeriodMs = 20;
inline constexpr uint32_t kNormalRefreshPeriodMs = 33;
inline constexpr uint32_t kNormalTouchPeriodMs = 10;
// Survival Mode: 10 frames per second, touch every 30 ms.
inline constexpr uint32_t kSurvivalRefreshPeriodMs = 100;
inline constexpr uint32_t kSurvivalTouchPeriodMs = 30;
// The automatic switch turns the mode on at this level when discharging, and
// off again once charging has brought the battery back up.
inline constexpr int kLowPowerAutoEnablePercent = 20;
inline constexpr int kLowPowerAutoDisablePercent = 80;

struct LowPowerState {
  // Low Power Mode is on.
  bool enabled = false;
  // Turn the mode on by itself at kLowPowerAutoEnablePercent.
  bool auto_enable = true;
  // The current session was started by the battery level, not the user, so
  // charging may end it again.
  bool enabled_automatically = false;
  // Wi-Fi was on when the mode switched it off and comes back afterwards.
  bool restore_wifi = false;
  // Survival Mode: Low Power Mode plus only the field guide, maps and radio,
  // and deep sleep whenever the screen is off.
  bool survival = false;
  // In every mode: deep sleep a little while after the screen goes off, unless
  // music plays, something records or the charger is in.
  bool deep_sleep_when_locked = true;
};

// Apps left on the home screen in Survival Mode (Settings stays, so the mode
// can be switched off again).
inline constexpr const char* kSurvivalApps[] = {"nature", "maps", "radio", "settings"};
// Seconds the screen stays off before the phone goes into deep sleep.
inline constexpr uint32_t kSurvivalDeepSleepDelayMs = 4000;
// Outside Survival Mode the phone waits a little longer, so switching the
// screen straight back on does not cost a restart.
inline constexpr uint32_t kLockedDeepSleepDelayMs = 10000;
inline constexpr int kSurvivalLockTimeoutSeconds = 20;
inline constexpr int kSurvivalBrightnessCapPercent = 25;
// CPU clock per mode. Battery life comes before smoothness: Low Power Mode
// halves the clock, Survival Mode quarters it (its screens are almost black and
// barely move, so they still draw quickly enough).
inline constexpr int kNormalCpuMhz = 360;
inline constexpr int kLowPowerCpuMhz = 180;
inline constexpr int kSurvivalCpuMhz = 90;

/**
 * @brief Loads the saved state from NVS. Call once after NVS is up.
 */
void InitLowPowerStorage();

/**
 * @brief Current state.
 * @return A copy of the state.
 */
LowPowerState GetLowPowerState();

/**
 * @brief Whether Low Power Mode is on.
 * @return true when on.
 */
bool IsLowPowerModeEnabled();

/**
 * @brief Switches the mode on or off at the user's request.
 * @param enabled New state.
 */
void SetLowPowerModeEnabled(bool enabled);

/**
 * @brief Whether Survival Mode is on.
 * @return true when on.
 */
bool IsSurvivalModeEnabled();

/**
 * @brief Switches Survival Mode; it brings Low Power Mode's limits with it.
 * @param enabled New state.
 */
void SetSurvivalModeEnabled(bool enabled);

/**
 * @brief Whether the phone goes into deep sleep once it is locked.
 * @return true when on (Survival Mode always sleeps).
 */
bool IsDeepSleepWhenLockedEnabled();

/**
 * @brief Switches deep sleep on locking.
 * @param enabled New setting.
 */
void SetDeepSleepWhenLocked(bool enabled);

/**
 * @brief Whether an app stays on the home screen in the current power mode.
 * @param app_id Catalog id.
 * @return true when shown.
 */
bool IsAppAllowedInPowerMode(const char* app_id);

/**
 * @brief Sets the CPU clock for the current mode through esp_pm.
 * @return The clock now in force, in MHz, or 0 when it could not be set.
 */
int ApplyCpuClockForPowerMode();

/**
 * @brief Records, in memory that survives deep sleep, that the phone went to
 *        sleep in Survival Mode.
 */
void MarkSurvivalSleep();

/**
 * @brief Whether this start is the phone waking from Survival Mode's deep
 *        sleep. Reads and clears the mark.
 * @return true for a survival wake.
 */
bool ConsumeSurvivalWake();

/**
 * @brief Chooses whether the mode turns on by itself on a low battery.
 * @param enabled New setting.
 */
void SetLowPowerAutoEnable(bool enabled);

/**
 * @brief Remembers whether Wi-Fi should come back when the mode ends.
 * @param restore true when Wi-Fi was switched off by the mode.
 */
void SetLowPowerWifiRestore(bool restore);

/**
 * @brief Feeds a battery reading to the automatic switch.
 * @param percent Charge level.
 * @param charging Whether external power is connected.
 */
void UpdateLowPowerForBattery(int percent, bool charging);

/**
 * @brief Registers the code that applies the mode to the hardware.
 *
 * Called on every change, from whichever task made it, outside any lock.
 *
 * @param listener Receives the new state.
 */
void SetLowPowerModeListener(std::function<void(bool enabled)> listener);

/**
 * @brief Caps a brightness to what the mode allows.
 * @param percent Requested brightness.
 * @return The brightness to apply.
 */
int LimitBrightnessForLowPower(int percent);

/**
 * @brief Applies the mode's brightness cap and auto-lock limit.
 * @param preferences Saved display preferences.
 * @return The preferences to act on; the saved ones are left alone.
 */
DisplayPreferences LimitDisplayPreferencesForLowPower(
    DisplayPreferences preferences);

}  // namespace lilygo_box::app
