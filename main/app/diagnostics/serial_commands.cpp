/*
 * @Description: Diagnostic commands over the USB serial port
 * @License: GPL 3.0
 */
#include "app/diagnostics/serial_commands.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "app/build_stamp.h"
#include "app/diagnostics/hardware_diagnostics.h"
#include "app/content_sorter.h"
#include "app/edition.h"
#include "app/power_saving.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "games/petdoku_engine.h"
#include "hal/device/common/camera_utils.h"
#include "sdkconfig.h"
#include "survival/jpeg_image.h"
#include "hal/lvgl_port.h"

namespace lilygo_box::app {
namespace {

constexpr uart_port_t kConsoleUart =
    static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM);
constexpr int kUartRxBufferBytes = 512;
// 16 kB: "svos shot" renders the whole screen on this task.
constexpr uint32_t kTaskStackBytes = 16 * 1024;
constexpr UBaseType_t kTaskPriority = 2;
constexpr size_t kLineLength = 220;
constexpr char kPrefix[] = "svos";
constexpr uint32_t kTaskSampleMs = 2000;
constexpr size_t kMaxSampledTasks = 48;
constexpr uint32_t kWifiScanTimeoutMs = 15000;
constexpr uint32_t kWifiScanPollMs = 250;
constexpr size_t kSdTestBlockBytes = 32 * 1024;
constexpr size_t kSdTestBlockCount = 32;
constexpr uint32_t kPetdokuBenchSeeds = 8;

SerialCommandTargets g_targets;

const char* FirmwareName(hal::WifiCoprocessorFirmware firmware) {
  switch (firmware) {
    case hal::WifiCoprocessorFirmware::kHosted:
      return "ESP-Hosted";
    case hal::WifiCoprocessorFirmware::kEspAt:
      return "ESP-AT (needs update)";
    case hal::WifiCoprocessorFirmware::kSilent:
      return "no answer";
    case hal::WifiCoprocessorFirmware::kUnknown:
      break;
  }
  return "not checked";
}

const char* StageName(hal::WifiCoprocessorUpdateStage stage) {
  switch (stage) {
    case hal::WifiCoprocessorUpdateStage::kChecking:
      return "checking";
    case hal::WifiCoprocessorUpdateStage::kEnteringDownloadMode:
      return "entering download mode";
    case hal::WifiCoprocessorUpdateStage::kNeedsBootButton:
      return "waiting for BOOT button (svos c6 boot)";
    case hal::WifiCoprocessorUpdateStage::kConnecting:
      return "connecting";
    case hal::WifiCoprocessorUpdateStage::kWriting:
      return "writing";
    case hal::WifiCoprocessorUpdateStage::kVerifying:
      return "verifying";
    case hal::WifiCoprocessorUpdateStage::kDone:
      return "done";
    case hal::WifiCoprocessorUpdateStage::kFailed:
      return "FAILED";
    case hal::WifiCoprocessorUpdateStage::kIdle:
      break;
  }
  return "idle";
}

void PrintStatus() {
  std::printf("svos: build %s, up %llu s, heap %u kB internal / %u kB PSRAM\n",
      BuildStamp(),
      static_cast<unsigned long long>(esp_timer_get_time() / 1000000),
      static_cast<unsigned>(
          heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

  if (g_targets.storage != nullptr) {
    char reason[200] = {};
    ReadHardwareDiagnostic(HardwareDiagnostic::kSdCard, reason, sizeof(reason));
    std::printf("svos: sd %s %s\n",
        g_targets.storage->IsSdCardMounted() ? "mounted" : "not mounted",
        reason);
  }

  if (g_targets.wifi != nullptr) {
    hal::WifiStatus wifi;
    if (g_targets.wifi->ReadWifiStatus(&wifi)) {
      std::printf("svos: wifi init=%d driver=%d running=%d connected=%d ip=%d "
                  "failed=%d error=%#x ssid=%s\n",
          wifi.init_task_running, wifi.driver_initialized, wifi.running,
          wifi.connected, wifi.got_ip, wifi.start_failed,
          static_cast<unsigned>(wifi.last_error), wifi.ssid);
    }
    hal::WifiCoprocessorStatus coprocessor;
    if (g_targets.wifi->ReadWifiCoprocessorStatus(&coprocessor)) {
      std::printf("svos: c6 firmware=%s stage=%s progress=%u%% image=%s "
                  "detail=%s\n",
          FirmwareName(coprocessor.firmware), StageName(coprocessor.stage),
          coprocessor.progress_percent,
          coprocessor.update_available ? "bundled" : "none",
          coprocessor.detail);
    }
  }

  if (g_targets.camera != nullptr) {
    hal::CameraPreviewFrameInfo frame;
    const bool has_frame = g_targets.camera->GetCameraPreviewFrameInfo(&frame);
    std::printf("svos: camera error=%d frames=%lu size=%lux%lu\n",
        static_cast<int>(g_targets.camera->GetCameraPreviewError()),
        static_cast<unsigned long>(has_frame ? frame.sequence : 0),
        static_cast<unsigned long>(has_frame ? frame.width : 0),
        static_cast<unsigned long>(has_frame ? frame.height : 0));
  }

  std::printf("svos: edition %s, setup %s\n",
      app::GetEdition() == app::Edition::kMini ? "mini" : "full",
      app::IsSetupCompleted() ? "done" : "pending");
  const app::LowPowerState low_power = app::GetLowPowerState();
  std::printf("svos: low power %s auto=%d by_battery=%d wifi_back=%d survival=%d\n",
      low_power.enabled ? "on" : "off", low_power.auto_enable,
      low_power.enabled_automatically, low_power.restore_wifi, low_power.survival);

  if (g_targets.battery != nullptr) {
    hal::BatteryManagementStatus battery;
    if (g_targets.battery->ReadBatteryManagementStatus(&battery)) {
      std::printf("svos: battery %d%% %d mV now %d mA avg %d mA, %d min to "
                  "empty, %s\n",
          battery.charge_percent, battery.voltage_mv, battery.current_ma,
          battery.average_current_ma, battery.time_to_empty_min,
          battery.charging ? "charging" : "discharging");
    }
  }
}

#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
/**
 * @brief Prints how much of the last two seconds each task spent running.
 */
void PrintTaskLoad() {
  std::unique_ptr<TaskStatus_t[]> before(
      new (std::nothrow) TaskStatus_t[kMaxSampledTasks]);
  std::unique_ptr<TaskStatus_t[]> after(
      new (std::nothrow) TaskStatus_t[kMaxSampledTasks]);
  if (before == nullptr || after == nullptr) {
    std::printf("svos: out of memory\n");
    return;
  }
  configRUN_TIME_COUNTER_TYPE total_before = 0;
  configRUN_TIME_COUNTER_TYPE total_after = 0;
  const UBaseType_t count_before =
      uxTaskGetSystemState(before.get(), kMaxSampledTasks, &total_before);
  vTaskDelay(pdMS_TO_TICKS(kTaskSampleMs));
  const UBaseType_t count_after =
      uxTaskGetSystemState(after.get(), kMaxSampledTasks, &total_after);
  const configRUN_TIME_COUNTER_TYPE elapsed = total_after - total_before;
  if (count_before == 0 || count_after == 0 || elapsed == 0) {
    std::printf("svos: no task statistics\n");
    return;
  }

  // The total is wall-clock time, so one fully busy core reads as 100 %.
  const configRUN_TIME_COUNTER_TYPE per_core = elapsed;
  for (UBaseType_t index = 0; index < count_after; ++index) {
    const TaskStatus_t& task = after[index];
    configRUN_TIME_COUNTER_TYPE earlier = 0;
    for (UBaseType_t other = 0; other < count_before; ++other) {
      if (before[other].xHandle == task.xHandle) {
        earlier = before[other].ulRunTimeCounter;
        break;
      }
    }
    const unsigned tenths = static_cast<unsigned>(
        static_cast<uint64_t>(task.ulRunTimeCounter - earlier) * 1000 /
        per_core);
    if (tenths == 0) {
      continue;
    }
    std::printf("svos: task %-16s %3u.%u %%  stack free %u\n",
        task.pcTaskName, tenths / 10, tenths % 10,
        static_cast<unsigned>(task.usStackHighWaterMark));
  }
}
#endif

/**
 * @brief Scans for Wi-Fi networks and lists what the radio hears.
 */
void ScanWifi() {
  hal::WifiScanStatus before;
  g_targets.wifi->ReadWifiScanStatus(&before);
  if (!g_targets.wifi->StartWifiScan()) {
    std::printf("svos: scan refused\n");
    return;
  }
  // The first call may only have switched Wi-Fi on; ask again once it is up.
  auto scan = std::make_unique<hal::WifiScanStatus>();
  for (uint32_t waited_ms = 0; waited_ms < kWifiScanTimeoutMs;
       waited_ms += kWifiScanPollMs) {
    vTaskDelay(pdMS_TO_TICKS(kWifiScanPollMs));
    g_targets.wifi->ReadWifiScanStatus(scan.get());
    if (scan->generation != before.generation && !scan->scan_running) {
      break;
    }
    hal::WifiStatus wifi;
    if (!scan->scan_running && g_targets.wifi->ReadWifiStatus(&wifi) &&
        wifi.driver_initialized && !wifi.init_task_running) {
      g_targets.wifi->StartWifiScan();
    }
  }
  std::printf("svos: scan %s, %u networks\n",
      scan->scan_failed ? "FAILED" : "done",
      static_cast<unsigned>(scan->network_count));
  for (size_t index = 0; index < scan->network_count; ++index) {
    const hal::WifiNetworkInfo& network = scan->networks[index];
    std::printf("svos:   %-32s %4d dBm ch %d%s\n", network.ssid, network.rssi,
        network.channel, network.secure ? "" : " open");
  }
}

/**
 * @brief Writes a file to the SD card, reads it back and removes it.
 */
void TestSdCard() {
  if (!g_targets.storage->EnsureSdCardMounted()) {
    std::printf("svos: sd not mounted\n");
    return;
  }
  char path[96];
  std::snprintf(path, sizeof(path), "%s/svos_selftest.tmp",
      g_targets.storage->SdCardBasePath());
  auto block = std::make_unique<uint8_t[]>(kSdTestBlockBytes);
  const int64_t started_us = esp_timer_get_time();
  std::FILE* file = std::fopen(path, "wb");
  bool passed = file != nullptr;
  for (size_t index = 0; passed && index < kSdTestBlockCount; ++index) {
    std::memset(block.get(), static_cast<int>(index + 1), kSdTestBlockBytes);
    passed = std::fwrite(block.get(), 1, kSdTestBlockBytes, file) ==
        kSdTestBlockBytes;
  }
  if (file != nullptr) {
    passed = std::fclose(file) == 0 && passed;
  }
  const int64_t written_us = esp_timer_get_time();
  file = passed ? std::fopen(path, "rb") : nullptr;
  passed = file != nullptr;
  for (size_t index = 0; passed && index < kSdTestBlockCount; ++index) {
    passed = std::fread(block.get(), 1, kSdTestBlockBytes, file) ==
        kSdTestBlockBytes;
    for (size_t byte = 0; passed && byte < kSdTestBlockBytes; ++byte) {
      passed = block[byte] == static_cast<uint8_t>(index + 1);
    }
  }
  if (file != nullptr) {
    std::fclose(file);
  }
  const int64_t read_us = esp_timer_get_time();
  std::remove(path);
  const unsigned kilobytes = kSdTestBlockBytes * kSdTestBlockCount / 1024;
  std::printf("svos: sd test %s (%u kB: write %lld ms, read %lld ms)\n",
      passed ? "passed" : "FAILED", kilobytes,
      static_cast<long long>((written_us - started_us) / 1000),
      static_cast<long long>((read_us - written_us) / 1000));
}

/**
 * @brief Times puzzle generation for every Petdoku size on this CPU.
 */
void BenchPetdoku() {
  for (int size = games::kPetdokuMinSize; size <= games::kPetdokuMaxSize;
       ++size) {
    int64_t worst_us = 0;
    int64_t total_us = 0;
    int generated = 0;
    for (uint32_t seed = 1; seed <= kPetdokuBenchSeeds; ++seed) {
      games::PetdokuPuzzle puzzle;
      const int64_t started_us = esp_timer_get_time();
      generated += games::GeneratePetdoku(size, seed, 800, &puzzle) ? 1 : 0;
      const int64_t spent_us = esp_timer_get_time() - started_us;
      total_us += spent_us;
      worst_us = spent_us > worst_us ? spent_us : worst_us;
    }
    std::printf("svos: petdoku %dx%d: %d/%lu, avg %lld ms, worst %lld ms\n",
        size, size, generated, static_cast<unsigned long>(kPetdokuBenchSeeds),
        static_cast<long long>(total_us / kPetdokuBenchSeeds / 1000),
        static_cast<long long>(worst_us / 1000));
  }
}

void PrintHelp() {
  std::printf(
      "svos: commands\n"
      "svos:   svos status            what the device sees\n"
      "svos:   svos tasks             CPU share of each task over 2 s\n"
      "svos:   svos wifi on|off|scan  switch Wi-Fi, list networks in range\n"
      "svos:   svos sd test           write, read back and delete a test file\n"
      "svos:   svos petdoku           time puzzle generation per size\n"
      "svos:   svos c6 identify       which firmware the Wi-Fi chip runs\n"
      "svos:   svos c6 rehearse       reach its download mode, write nothing\n"
      "svos:   svos c6 install        replace ESP-AT with ESP-Hosted\n"
      "svos:   svos c6 boot           BOOT button is being held\n"
      "svos:   svos c6 at A;B;...     send AT commands (no spaces), log replies\n"
      "svos:   svos camera on|off|wide|normal|fill  preview and its zoom\n"
      "svos:   svos jpeg              colour round trip through encoder+decoder\n"
      "svos:   svos lowpower on|off   switch Low Power Mode\n"
      "svos:   svos survival on|off   switch Survival Mode (deep sleep when dark)\n"
      "svos:   svos setup             run the setup assistant at next start\n"
      "svos:   svos edition full|mini choose the edition\n"
      "svos:   svos sort              file SD card downloads into place\n"
      "svos:   svos tap X Y [ms]      touch the screen\n"
      "svos:   svos swipe X1 Y1 X2 Y2 [ms]  drag a finger\n"
      "svos:   svos shot [quality]    screenshot as base64 JPEG\n");
}

void StartCoprocessor(hal::WifiCoprocessorUpdateMode mode) {
  if (g_targets.wifi == nullptr) {
    std::printf("svos: no Wi-Fi on this board\n");
    return;
  }
  std::printf("svos: %s\n",
      g_targets.wifi->StartWifiCoprocessorUpdate(mode)
          ? "started; follow with svos status"
          : "not started (already running?)");
}

// ── SurvivalOS diagnostics: touches and screenshots ─────────────────────────

// Finger down at the start, moved in steps, lifted at the end.
void Swipe(int x1, int y1, int x2, int y2, int duration_ms) {
  hal::LvglPort* lvgl = g_targets.lvgl;
  const int steps = std::max(1, duration_ms / 15);
  for (int i = 0; i <= steps; ++i) {
    lvgl->InjectTouch(true, x1 + (x2 - x1) * i / steps, y1 + (y2 - y1) * i / steps);
    vTaskDelay(pdMS_TO_TICKS(15));
  }
  lvgl->InjectTouch(false, x2, y2);
  vTaskDelay(pdMS_TO_TICKS(40));
}

void PrintBase64(const uint8_t* data, size_t size) {
  static const char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  char line[100];
  size_t used = 0;
  for (size_t i = 0; i < size; i += 3) {
    const uint32_t a = data[i];
    const uint32_t b = i + 1 < size ? data[i + 1] : 0;
    const uint32_t c = i + 2 < size ? data[i + 2] : 0;
    const uint32_t triple = (a << 16) | (b << 8) | c;
    line[used++] = kAlphabet[(triple >> 18) & 63];
    line[used++] = kAlphabet[(triple >> 12) & 63];
    line[used++] = i + 1 < size ? kAlphabet[(triple >> 6) & 63] : '=';
    line[used++] = i + 2 < size ? kAlphabet[triple & 63] : '=';
    if (used >= 76 || i + 3 >= size) {
      line[used] = '\0';
      std::printf("%s\n", line);
      used = 0;
    }
  }
}

// Screenshot as a JPEG in base64 between two marker lines; the Mac side
// (tools/device_shot.py) cuts it out of the console log.
void PrintScreenshot(int quality) {
  int width = 0;
  int height = 0;
  uint8_t* pixels = g_targets.lvgl->CaptureScreen(&width, &height);
  if (pixels == nullptr) {
    std::printf("svos: shot failed (no memory or display asleep)\n");
    return;
  }
  uint8_t* jpeg = nullptr;
  size_t jpeg_size = 0;
  const bool ok = survival::EncodeJpegRgb565(pixels, static_cast<uint32_t>(width),
      static_cast<uint32_t>(height), quality, &jpeg, &jpeg_size);
  heap_caps_free(pixels);
  if (!ok) {
    std::printf("svos: shot encode failed\n");
    return;
  }
  std::printf("svos-shot-begin %dx%d %u\n", width, height, static_cast<unsigned>(jpeg_size));
  PrintBase64(jpeg, jpeg_size);
  std::printf("svos-shot-end\n");
  free(jpeg);
}

void RunCommand(char* line) {
  // Opening the port can leave a stray byte in front of the first command.
  line = std::strstr(line, kPrefix);
  if (line == nullptr) {
    return;
  }
  char* words[8] = {};
  size_t count = 0;
  for (char* word = std::strtok(line, " \t\r\n");
       word != nullptr && count < 8; word = std::strtok(nullptr, " \t\r\n")) {
    words[count++] = word;
  }
  if (count < 2 || std::strcmp(words[0], kPrefix) != 0) {
    return;
  }
  const char* command = words[1];
  const char* argument = count > 2 ? words[2] : "";

  if (std::strcmp(command, "status") == 0) {
    PrintStatus();
  } else if (std::strcmp(command, "tap") == 0 && g_targets.lvgl != nullptr && count >= 4) {
    const int x = std::atoi(words[2]);
    const int y = std::atoi(words[3]);
    const int hold = count >= 5 ? std::atoi(words[4]) : 80;
    g_targets.lvgl->InjectTouch(true, x, y);
    vTaskDelay(pdMS_TO_TICKS(std::max(20, hold)));
    g_targets.lvgl->InjectTouch(false, x, y);
    vTaskDelay(pdMS_TO_TICKS(40));
    std::printf("svos: tap %d,%d\n", x, y);
  } else if (std::strcmp(command, "swipe") == 0 && g_targets.lvgl != nullptr && count >= 6) {
    Swipe(std::atoi(words[2]), std::atoi(words[3]), std::atoi(words[4]),
        std::atoi(words[5]), count >= 7 ? std::atoi(words[6]) : 250);
    std::printf("svos: swipe done\n");
  } else if (std::strcmp(command, "shot") == 0 && g_targets.lvgl != nullptr) {
    PrintScreenshot(count >= 3 ? std::atoi(words[2]) : 70);
  } else if (std::strcmp(command, "tasks") == 0) {
#if CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS
    PrintTaskLoad();
#else
    std::printf("svos: task statistics are not built in\n");
#endif
  } else if (std::strcmp(command, "wifi") == 0 && g_targets.wifi != nullptr &&
             std::strcmp(argument, "scan") == 0) {
    ScanWifi();
  } else if (std::strcmp(command, "lowpower") == 0 &&
             (std::strcmp(argument, "on") == 0 ||
                 std::strcmp(argument, "off") == 0)) {
    app::SetLowPowerModeEnabled(std::strcmp(argument, "on") == 0);
    std::printf("svos: low power %s\n",
        app::IsLowPowerModeEnabled() ? "on" : "off");
  } else if (std::strcmp(command, "survival") == 0 &&
             (std::strcmp(argument, "on") == 0 ||
                 std::strcmp(argument, "off") == 0)) {
    app::SetSurvivalModeEnabled(std::strcmp(argument, "on") == 0);
    std::printf("svos: survival %s\n", app::IsSurvivalModeEnabled() ? "on" : "off");
  } else if (std::strcmp(command, "setup") == 0) {
    app::ResetSetup();
    std::printf("svos: setup assistant runs at the next start\n");
  } else if (std::strcmp(command, "edition") == 0 &&
             (std::strcmp(argument, "full") == 0 ||
                 std::strcmp(argument, "mini") == 0)) {
    app::SetEdition(std::strcmp(argument, "full") == 0 ? app::Edition::kFull
                                                       : app::Edition::kMini);
    std::printf("svos: edition %s from the next start\n", argument);
  } else if (std::strcmp(command, "sort") == 0 &&
             g_targets.storage != nullptr) {
    if (!g_targets.storage->EnsureSdCardMounted()) {
      std::printf("svos: sort: no SD card\n");
    } else {
      std::printf("svos: sort moved %u file(s)\n",
          static_cast<unsigned>(
              app::SortCardContent(g_targets.storage->SdCardBasePath())));
    }
  } else if (std::strcmp(command, "jpeg") == 0) {
    char report[96];
    const bool passed = survival::RunJpegColorSelfTest(report, sizeof(report));
    std::printf("svos: jpeg %s %s\n", passed ? "OK" : "FAILED", report);
  } else if (std::strcmp(command, "petdoku") == 0) {
    BenchPetdoku();
  } else if (std::strcmp(command, "sd") == 0 && g_targets.storage != nullptr) {
    TestSdCard();
  } else if (std::strcmp(command, "wifi") == 0 && g_targets.wifi != nullptr) {
    const bool enable = std::strcmp(argument, "on") == 0;
    std::printf("svos: wifi %s -> %s\n", enable ? "on" : "off",
        g_targets.wifi->SetWifiEnabled(enable) ? "accepted" : "refused");
  } else if (std::strcmp(command, "c6") == 0) {
    if (std::strcmp(argument, "identify") == 0) {
      StartCoprocessor(hal::WifiCoprocessorUpdateMode::kIdentify);
    } else if (std::strcmp(argument, "rehearse") == 0) {
      StartCoprocessor(hal::WifiCoprocessorUpdateMode::kRehearse);
    } else if (std::strcmp(argument, "install") == 0) {
      StartCoprocessor(hal::WifiCoprocessorUpdateMode::kInstall);
    } else if (std::strcmp(argument, "at") == 0 && count > 3 &&
               g_targets.wifi != nullptr) {
      std::printf("svos: %s\n",
          g_targets.wifi->StartWifiCoprocessorExperiment(words[3])
              ? "started"
              : "not started (already running?)");
    } else if (std::strcmp(argument, "boot") == 0 &&
               g_targets.wifi != nullptr) {
      std::printf("svos: %s\n",
          g_targets.wifi->ConfirmWifiCoprocessorBootButton()
              ? "continuing"
              : "nothing is waiting for the button");
    } else {
      PrintHelp();
    }
  } else if (std::strcmp(command, "camera") == 0 &&
             g_targets.camera != nullptr) {
    using hal::camera_utils::PreviewZoom;
    const bool wide = std::strcmp(argument, "wide") == 0;
    const bool normal = std::strcmp(argument, "normal") == 0;
    const bool fill = std::strcmp(argument, "fill") == 0;
    if (wide || normal || fill) {
      // Takes effect with the next frame when the preview is already running.
      hal::camera_utils::SetPreviewZoom(wide     ? PreviewZoom::kWide
                                        : normal ? PreviewZoom::kNormal
                                                 : PreviewZoom::kFill);
    }
    if (std::strcmp(argument, "on") == 0 || wide || normal || fill) {
      std::printf("svos: camera start -> %s\n",
          g_targets.camera->StartCameraPreview() ? "accepted" : "refused");
    } else {
      std::printf("svos: camera stop -> %s\n",
          g_targets.camera->StopCameraPreview() ? "stopped" : "failed");
    }
  } else {
    PrintHelp();
  }
  std::fflush(stdout);
}

void SerialCommandTask(void* context) {
  (void)context;
  char line[kLineLength] = {};
  size_t length = 0;
  while (true) {
    uint8_t byte = 0;
    if (uart_read_bytes(kConsoleUart, &byte, 1, pdMS_TO_TICKS(200)) != 1) {
      continue;
    }
    if (byte == '\n' || byte == '\r') {
      if (length > 0) {
        line[length] = '\0';
        RunCommand(line);
        length = 0;
      }
      continue;
    }
    if (length + 1 < sizeof(line)) {
      line[length++] = static_cast<char>(byte);
    } else {
      // Too long to be a command: drop it and start over at the next line.
      length = 0;
    }
  }
}

}  // namespace

void StartSerialCommands(const SerialCommandTargets& targets) {
  static bool started = false;
  if (started) {
    return;
  }
  g_targets = targets;
  // Receiving needs the UART driver; printing keeps going through the console
  // as before.
  if (!uart_is_driver_installed(kConsoleUart) &&
      uart_driver_install(kConsoleUart, kUartRxBufferBytes, 0, 0, nullptr, 0) !=
          ESP_OK) {
    return;
  }
  started = xTaskCreate(SerialCommandTask, "svos_serial", kTaskStackBytes,
                nullptr, kTaskPriority, nullptr) == pdPASS;
}

}  // namespace lilygo_box::app
