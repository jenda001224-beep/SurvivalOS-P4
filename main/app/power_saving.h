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
};

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
