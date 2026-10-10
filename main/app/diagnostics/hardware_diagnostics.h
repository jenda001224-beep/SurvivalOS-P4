/*
 * @Description: On-device record of why the SD card or the Wi-Fi chip failed
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace lilygo_box::app {

// The reason a storage or radio bring-up failed normally only reaches the
// serial console. These helpers keep the last warnings and errors that ESP-IDF
// logged during such an attempt, so the UI can show them on the device itself.

/**
 * @brief Starts recording ESP-IDF warnings and errors. Call once, early.
 */
void InstallLogCapture();

/**
 * @brief Returns a marker for "now" to pass to CollectLogLinesSince.
 */
uint32_t LogCaptureMark();

/**
 * @brief Joins the warnings and errors logged since a marker into one string.
 * @param mark Marker from LogCaptureMark.
 * @param buffer Receives the lines, newest last, separated by newlines.
 * @param buffer_size Size of buffer.
 */
void CollectLogLinesSince(uint32_t mark, char* buffer, size_t buffer_size);

enum class HardwareDiagnostic : uint8_t { kSdCard, kWifi, kCount };

/**
 * @brief Stores the outcome text of the latest attempt, empty on success.
 */
void SetHardwareDiagnostic(HardwareDiagnostic which, const char* text);

/**
 * @brief Reads the text stored by SetHardwareDiagnostic.
 */
void ReadHardwareDiagnostic(
    HardwareDiagnostic which, char* buffer, size_t buffer_size);

}  // namespace lilygo_box::app
