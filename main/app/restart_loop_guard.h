/*
 * @Description: Detects a device that keeps restarting itself shortly after boot
 * @License: GPL 3.0
 */
#pragma once

namespace lilygo_box::app {

/**
 * @brief Tells whether this boot is one of several self-inflicted restarts in a row.
 *
 * Counts restarts caused by the firmware itself (software reset, panic,
 * watchdog) that follow each other within a minute of booting. The count lives
 * in memory that survives those resets but not a power cycle, and it is cleared
 * once the device has stayed up for a minute. Call it exactly once per boot.
 *
 * @return true on the third such boot in a row.
 */
bool IsDeviceInRestartLoop();

}  // namespace lilygo_box::app
