/*
 * @Description: On-device record of why the SD card or the Wi-Fi chip failed
 * @License: GPL 3.0
 */
#include "app/diagnostics/hardware_diagnostics.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace lilygo_box::app {
namespace {

constexpr size_t kLineCount = 10;
constexpr size_t kLineLength = 96;
constexpr size_t kDiagnosticLength = 200;

struct CapturedLine {
  uint32_t sequence = 0;
  char text[kLineLength] = {};
};

portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED;
CapturedLine g_lines[kLineCount];
uint32_t g_sequence = 0;
vprintf_like_t g_previous_vprintf = nullptr;
char g_diagnostics[static_cast<size_t>(HardwareDiagnostic::kCount)]
                  [kDiagnosticLength] = {};

/**
 * @brief Tells whether a log format string is for a warning or an error.
 *
 * With log version 1 and colours off, ESP-IDF passes the whole line as one
 * format string that starts with the level letter: "E (%lu) %s: ...".
 */
bool IsWarningOrError(const char* format) {
  return (format[0] == 'E' || format[0] == 'W') && format[1] == ' ' &&
      format[2] == '(';
}

int CaptureVprintf(const char* format, va_list arguments) {
  if (format != nullptr && IsWarningOrError(format)) {
    char line[kLineLength];
    va_list copy;
    va_copy(copy, arguments);
    int length = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (length > 0) {
      length = length < static_cast<int>(sizeof(line))
          ? length
          : static_cast<int>(sizeof(line)) - 1;
      while (length > 0 &&
          (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
      }
      taskENTER_CRITICAL(&g_lock);
      CapturedLine& slot = g_lines[g_sequence % kLineCount];
      slot.sequence = ++g_sequence;
      std::memcpy(slot.text, line, static_cast<size_t>(length) + 1);
      taskEXIT_CRITICAL(&g_lock);
    }
  }
  return g_previous_vprintf != nullptr ? g_previous_vprintf(format, arguments)
                                       : vprintf(format, arguments);
}

}  // namespace

void InstallLogCapture() {
  if (g_previous_vprintf == nullptr) {
    g_previous_vprintf = esp_log_set_vprintf(CaptureVprintf);
  }
}

uint32_t LogCaptureMark() {
  taskENTER_CRITICAL(&g_lock);
  const uint32_t mark = g_sequence;
  taskEXIT_CRITICAL(&g_lock);
  return mark;
}

void CollectLogLinesSince(uint32_t mark, char* buffer, size_t buffer_size) {
  if (buffer == nullptr || buffer_size == 0) {
    return;
  }
  buffer[0] = '\0';
  size_t used = 0;
  // Oldest first, so the text reads in the order things went wrong.
  for (size_t step = 0; step < kLineCount; ++step) {
    char line[kLineLength] = {};
    taskENTER_CRITICAL(&g_lock);
    const uint32_t newest = g_sequence;
    const uint32_t back = static_cast<uint32_t>(kLineCount - 1 - step);
    const uint32_t wanted = newest > back ? newest - back : 0;
    if (wanted > mark && wanted <= newest) {
      const CapturedLine& slot = g_lines[(wanted - 1) % kLineCount];
      if (slot.sequence == wanted) {
        std::memcpy(line, slot.text, sizeof(line));
      }
    }
    taskEXIT_CRITICAL(&g_lock);
    if (line[0] == '\0') {
      continue;
    }
    // Drop the "E (12345) " prefix; the tag and message are what matter.
    const char* text = std::strchr(line, ')');
    text = text != nullptr && text[1] == ' ' ? text + 2 : line;
    const int written = std::snprintf(buffer + used, buffer_size - used,
        "%s%s", used > 0 ? "\n" : "", text);
    if (written <= 0 || static_cast<size_t>(written) >= buffer_size - used) {
      break;
    }
    used += static_cast<size_t>(written);
  }
}

void SetHardwareDiagnostic(HardwareDiagnostic which, const char* text) {
  const size_t index = static_cast<size_t>(which);
  if (index >= static_cast<size_t>(HardwareDiagnostic::kCount)) {
    return;
  }
  char copy[kDiagnosticLength] = {};
  if (text != nullptr) {
    std::snprintf(copy, sizeof(copy), "%s", text);
  }
  taskENTER_CRITICAL(&g_lock);
  std::memcpy(g_diagnostics[index], copy, sizeof(copy));
  taskEXIT_CRITICAL(&g_lock);
}

void ReadHardwareDiagnostic(
    HardwareDiagnostic which, char* buffer, size_t buffer_size) {
  if (buffer == nullptr || buffer_size == 0) {
    return;
  }
  buffer[0] = '\0';
  const size_t index = static_cast<size_t>(which);
  if (index >= static_cast<size_t>(HardwareDiagnostic::kCount)) {
    return;
  }
  char copy[kDiagnosticLength] = {};
  taskENTER_CRITICAL(&g_lock);
  std::memcpy(copy, g_diagnostics[index], sizeof(copy));
  taskEXIT_CRITICAL(&g_lock);
  std::snprintf(buffer, buffer_size, "%s", copy);
}

}  // namespace lilygo_box::app
