/*
 * @Description: Diagnostic commands over the USB serial port
 * @License: GPL 3.0
 */
#pragma once

#include <functional>

#include "hal/providers/providers.h"

namespace lilygo_box::hal {
class LvglPort;
}  // namespace lilygo_box::hal

namespace lilygo_box::app {

// SurvivalOS: lets a computer on the P4.U port ask the device what it sees
// and start the few actions whose logs matter when something does not work,
// without anybody having to time taps on the screen against a log recording.
// Every command is a line that starts with "svos"; "svos help" lists them.

struct SerialCommandTargets {
  hal::WifiProvider* wifi = nullptr;
  hal::StorageProvider* storage = nullptr;
  hal::CameraProvider* camera = nullptr;
  hal::BatteryManagementProvider* battery = nullptr;
  // SurvivalOS diagnostics: touches and screenshots over the console.
  hal::LvglPort* lvgl = nullptr;
  hal::RtcProvider* rtc = nullptr;
  // Acts as a short press of the power button (screen off / on).
  std::function<void()> power_press;
  // Lock state (0 unlocked, 1 lock screen lit, 2 asleep) and brightness.
  std::function<void(int*, int*)> screen_state;
};

/**
 * @brief Starts the task that listens for commands on the console UART.
 * @param targets Hardware providers the commands act on; any may be null.
 */
void StartSerialCommands(const SerialCommandTargets& targets);

}  // namespace lilygo_box::app
