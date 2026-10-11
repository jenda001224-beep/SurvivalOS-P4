/*
 * @Description: Diagnostic commands over the USB serial port
 * @License: GPL 3.0
 */
#pragma once

#include "hal/providers/providers.h"

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
};

/**
 * @brief Starts the task that listens for commands on the console UART.
 * @param targets Hardware providers the commands act on; any may be null.
 */
void StartSerialCommands(const SerialCommandTargets& targets);

}  // namespace lilygo_box::app
