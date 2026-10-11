/*
 * @Description: T-Display-P4 Wi-Fi co-processor firmware service
 * @License: GPL 3.0
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>

#include "app/diagnostics/hardware_diagnostics.h"
#include "base/logger.h"
#include "driver/sdmmc_host.h"
#include "esp_loader.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/device/common/coprocessor_sdio.h"
#include "hal/device/t_display_p4/device.h"
#include "nvs.h"

// SurvivalOS: the T-Display-P4 V1.0 left the factory with ESP-AT on its
// ESP32-C6 (the vendor now files that firmware under "deprecated"), while this
// firmware, like the vendor's own, drives the chip through ESP-Hosted. The two
// do not understand each other, so Wi-Fi never comes up, and because a failed
// ESP-Hosted start restarts the system, switching Wi-Fi on used to reboot the
// phone. The vendor's remedy is a USB-UART adapter on the "C6 UART" connector.
//
// The phone can do it alone instead. The ESP32-C6's ROM accepts firmware over
// SDIO, the bus the two chips already share, and Espressif's esp-serial-flasher
// implements that for exactly this pair of chips and pins. The one thing the
// ESP32-P4 cannot reach is the ESP32-C6's BOOT strap, so download mode is
// entered by asking ESP-AT itself: it can write the register that forces a
// download boot and then restart. Holding the BOOT button remains as a
// fallback.

namespace lilygo_box::hal {
namespace {

namespace board = lilygo_device_driver::t_display_p4;

// Vendor's ESP-Hosted build for this board, packed by
// tools/pack_coprocessor_firmware.py.
extern const uint8_t kPackedImageStart[] asm(
    "_binary_coprocessor_firmware_start");
extern const uint8_t kPackedImageEnd[] asm("_binary_coprocessor_firmware_end");

constexpr char kPackedImageMagic[] = "SVC6";
constexpr size_t kPackedImageHeaderBytes = 40;
constexpr size_t kMd5HexLength = 32;

constexpr uint32_t kTaskStackBytes = 12 * 1024;
constexpr UBaseType_t kTaskPriority = 3;
constexpr uint32_t kSdioFrequencyKhz = SDMMC_FREQ_DEFAULT;
constexpr uint32_t kResetHoldMs = 100;
// The firmware re-initialises the chip's SDIO slave while it starts, which
// drops a card that was set up too early. ESP-Hosted waits this long too.
constexpr uint32_t kFirmwareBootMs = 1500;
constexpr int kLinkAttempts = 5;
constexpr uint32_t kLinkRetryMs = 300;
constexpr uint32_t kReadyTimeoutMs = 2500;
constexpr uint32_t kCommandTimeoutMs = 2000;
constexpr uint32_t kDownloadBootMs = 500;
constexpr int kSoftwareEntryAttempts = 2;
constexpr uint32_t kBootButtonTimeoutMs = 120 * 1000;
constexpr uint32_t kWifiStopTimeoutMs = 10 * 1000;
constexpr uint32_t kPollMs = 50;
constexpr uint32_t kFlashBlockBytes = 4096;

// ESP32-C6 LP_AON_SYS_CFG_REG; bit 30 makes the next restart enter the ROM
// download mode (soc/lp_aon_reg.h of the esp32c6 target).
constexpr uint32_t kForceDownloadRegister = 0x600B1034;
constexpr uint32_t kForceDownloadBit = 1U << 30;

constexpr char kNvsNamespace[] = "svos";
constexpr char kNvsHostedKey[] = "c6_hosted";

struct PackedImage {
  const uint8_t* stream = nullptr;
  uint32_t stream_size = 0;
  uint32_t image_size = 0;
  char md5[kMd5HexLength + 1] = {};
};

bool ReadPackedImage(PackedImage* image) {
  const size_t total = static_cast<size_t>(kPackedImageEnd - kPackedImageStart);
  if (total <= kPackedImageHeaderBytes ||
      std::memcmp(kPackedImageStart, kPackedImageMagic, 4) != 0) {
    return false;
  }
  std::memcpy(&image->image_size, kPackedImageStart + 4, sizeof(uint32_t));
  std::memcpy(image->md5, kPackedImageStart + 8, kMd5HexLength);
  image->md5[kMd5HexLength] = '\0';
  image->stream = kPackedImageStart + kPackedImageHeaderBytes;
  image->stream_size = static_cast<uint32_t>(total - kPackedImageHeaderBytes);
  return image->image_size > 0 && image->image_size % 4 == 0;
}

bool SetCoprocessorEnabled(void* context, bool enabled) {
  return static_cast<lilygo_device_driver::TDisplayP4Driver*>(context)
      ->SetEsp32c6PowerEnabled(enabled);
}

CoprocessorSdioConfig MakeSdioConfig(
    lilygo_device_driver::TDisplayP4Driver* driver) {
  CoprocessorSdioConfig config;
  config.slot = SDMMC_HOST_SLOT_1;
  config.clk = board::gpio::esp32c6::kSdioClk;
  config.cmd = board::gpio::esp32c6::kSdioCmd;
  config.d0 = board::gpio::esp32c6::kSdioD0;
  config.d1 = board::gpio::esp32c6::kSdioD1;
  config.d2 = board::gpio::esp32c6::kSdioD2;
  config.d3 = board::gpio::esp32c6::kSdioD3;
  config.set_enabled = SetCoprocessorEnabled;
  config.context = driver;
  return config;
}

bool ReadHostedVerified() {
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  uint8_t value = 0;
  const bool verified =
      nvs_get_u8(handle, kNvsHostedKey, &value) == ESP_OK && value == 1;
  nvs_close(handle);
  return verified;
}

void WriteHostedVerified(bool verified) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return;
  }
  nvs_set_u8(handle, kNvsHostedKey, verified ? 1 : 0);
  nvs_commit(handle);
  nvs_close(handle);
}

/**
 * @brief Pulls the first line that says something out of an AT+GMR reply.
 */
void CopyVersionLine(const std::string& reply, char* buffer, size_t size) {
  buffer[0] = '\0';
  size_t start = reply.find("AT version:");
  if (start == std::string::npos) {
    start = reply.find_first_not_of("\r\n");
  }
  if (start == std::string::npos) {
    return;
  }
  const size_t end = reply.find_first_of("\r\n", start);
  std::snprintf(buffer, size, "ESP-AT %s",
      reply.substr(start, end == std::string::npos ? end : end - start)
          .c_str());
}

/**
 * @brief Resets the co-processor and finds out which firmware answers.
 *
 * When it is ESP-AT the link is left open and usable for commands.
 */
WifiCoprocessorFirmware Identify(CoprocessorSdio* sdio, EspAtSdioLink* link,
    char* version, size_t version_size) {
  version[0] = '\0';
  if (!sdio->Open() || !sdio->Reset(kResetHoldMs)) {
    return WifiCoprocessorFirmware::kSilent;
  }
  vTaskDelay(pdMS_TO_TICKS(kFirmwareBootMs));

  bool link_ready = false;
  for (int attempt = 0; attempt < kLinkAttempts && !link_ready; ++attempt) {
    link_ready = sdio->InitCard(kSdioFrequencyKhz) && link->Start();
    if (!link_ready) {
      vTaskDelay(pdMS_TO_TICKS(kLinkRetryMs));
    }
  }
  if (!link_ready) {
    return WifiCoprocessorFirmware::kSilent;
  }

  // ESP-AT announces itself with "ready" as soon as it is up; ESP-Hosted
  // stays quiet until its own host driver opens the data path.
  std::string text;
  EspAtSdioLink::Result result =
      link->WaitFor("ready", kReadyTimeoutMs, &text);
  if (result == EspAtSdioLink::Result::kBusError) {
    return WifiCoprocessorFirmware::kSilent;
  }
  if (result != EspAtSdioLink::Result::kOk) {
    // It may have been up before the link was: ask.
    result = link->Command("AT", kCommandTimeoutMs / 2, nullptr);
    if (result != EspAtSdioLink::Result::kOk) {
      return WifiCoprocessorFirmware::kHosted;
    }
  }

  link->Command("ATE0", kCommandTimeoutMs, nullptr);
  std::string reply;
  if (link->Command("AT+GMR", kCommandTimeoutMs, &reply) ==
      EspAtSdioLink::Result::kOk) {
    CopyVersionLine(reply, version, version_size);
  }
  if (version[0] == '\0') {
    std::snprintf(version, version_size, "ESP-AT");
  }
  return WifiCoprocessorFirmware::kEspAt;
}

/**
 * @brief Has ESP-AT set the force-download bit and restart itself.
 */
bool EnterDownloadModeThroughAt(EspAtSdioLink* link) {
  char command[64];
  std::string reply;
  // Reading first proves the command exists before anything depends on it.
  std::snprintf(command, sizeof(command), "AT+SYSREG=0,%lu",
      static_cast<unsigned long>(kForceDownloadRegister));
  if (link->Command(command, kCommandTimeoutMs, &reply) !=
      EspAtSdioLink::Result::kOk) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Wi-Fi co-processor: %s was not accepted\n", command);
    return false;
  }
  const size_t value_at = reply.find("+SYSREG:");
  if (value_at == std::string::npos) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Wi-Fi co-processor: %s returned no value\n", command);
    return false;
  }
  const uint32_t value = static_cast<uint32_t>(
      std::strtoul(reply.c_str() + value_at + 8, nullptr, 0));

  std::snprintf(command, sizeof(command), "AT+SYSREG=1,%lu,%lu",
      static_cast<unsigned long>(kForceDownloadRegister),
      static_cast<unsigned long>(value | kForceDownloadBit));
  if (link->Command(command, kCommandTimeoutMs, &reply) !=
      EspAtSdioLink::Result::kOk) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Wi-Fi co-processor: %s was not accepted\n", command);
    return false;
  }
  // ESP-AT answers OK and then restarts; a reply lost to the restart is not
  // an error, a command that could not be sent is.
  const EspAtSdioLink::Result restart =
      link->Command("AT+RST", kCommandTimeoutMs / 4, &reply);
  if (restart == EspAtSdioLink::Result::kError) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Wi-Fi co-processor: AT+RST was not accepted\n");
    return false;
  }
  vTaskDelay(pdMS_TO_TICKS(kDownloadBootMs));
  return true;
}

}  // namespace

bool TDisplayP4Device::ReadWifiCoprocessorStatus(
    WifiCoprocessorStatus* status) {
  if (status == nullptr) {
    return false;
  }
  PackedImage image;
  status->firmware = wifi_coprocessor_.firmware.load();
  status->stage = wifi_coprocessor_.stage.load();
  status->update_available = ReadPackedImage(&image);
  status->progress_percent = wifi_coprocessor_.progress_percent.load();
  taskENTER_CRITICAL(&wifi_coprocessor_.lock);
  std::memcpy(status->detail, wifi_coprocessor_.detail, sizeof(status->detail));
  taskEXIT_CRITICAL(&wifi_coprocessor_.lock);
  status->detail[sizeof(status->detail) - 1] = '\0';
  return true;
}

bool TDisplayP4Device::StartWifiCoprocessorUpdate(
    WifiCoprocessorUpdateMode mode) {
  bool expected = false;
  if (!wifi_coprocessor_.task_running.compare_exchange_strong(expected, true)) {
    return false;
  }
  wifi_coprocessor_.mode.store(mode);
  wifi_coprocessor_.progress_percent.store(0);
  wifi_coprocessor_.boot_button_confirmed.store(false);
  SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kChecking, "");
  if (xTaskCreate(WifiCoprocessorTaskEntry, "c6_update", kTaskStackBytes, this,
          kTaskPriority, nullptr) != pdPASS) {
    SetWifiCoprocessorStage(
        WifiCoprocessorUpdateStage::kFailed, "Out of memory");
    wifi_coprocessor_.task_running.store(false);
    return false;
  }
  return true;
}

bool TDisplayP4Device::StartWifiCoprocessorExperiment(const char* script) {
  if (script == nullptr || script[0] == '\0') {
    return false;
  }
  bool expected = false;
  if (!wifi_coprocessor_.task_running.compare_exchange_strong(expected, true)) {
    return false;
  }
  std::snprintf(wifi_coprocessor_.experiment,
      sizeof(wifi_coprocessor_.experiment), "%s", script);
  SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kChecking, "");
  if (xTaskCreate(WifiCoprocessorTaskEntry, "c6_update", kTaskStackBytes, this,
          kTaskPriority, nullptr) != pdPASS) {
    wifi_coprocessor_.experiment[0] = '\0';
    SetWifiCoprocessorStage(
        WifiCoprocessorUpdateStage::kFailed, "Out of memory");
    wifi_coprocessor_.task_running.store(false);
    return false;
  }
  return true;
}

bool TDisplayP4Device::ConfirmWifiCoprocessorBootButton() {
  if (wifi_coprocessor_.stage.load() !=
      WifiCoprocessorUpdateStage::kNeedsBootButton) {
    return false;
  }
  wifi_coprocessor_.boot_button_confirmed.store(true);
  return true;
}

void TDisplayP4Device::SetWifiCoprocessorStage(
    WifiCoprocessorUpdateStage stage, const char* detail) {
  if (detail != nullptr) {
    char copy[sizeof(wifi_coprocessor_.detail)] = {};
    std::snprintf(copy, sizeof(copy), "%s", detail);
    taskENTER_CRITICAL(&wifi_coprocessor_.lock);
    std::memcpy(wifi_coprocessor_.detail, copy, sizeof(copy));
    taskEXIT_CRITICAL(&wifi_coprocessor_.lock);
    if (detail[0] != '\0') {
      LogMessage(stage == WifiCoprocessorUpdateStage::kFailed
                     ? LogLevel::kWarning
                     : LogLevel::kInfo,
          __FILE__, __LINE__, "Wi-Fi co-processor: %s\n", detail);
    }
  }
  wifi_coprocessor_.stage.store(stage);
}

void TDisplayP4Device::WifiCoprocessorTaskEntry(void* context) {
  auto* self = static_cast<TDisplayP4Device*>(context);
  if (self != nullptr) {
    self->RunWifiCoprocessorTask();
    self->wifi_coprocessor_.task_running.store(false);
  }
  vTaskDelete(nullptr);
}

void TDisplayP4Device::RunWifiCoprocessorExperiment() {
  char script[sizeof(wifi_coprocessor_.experiment)];
  std::snprintf(script, sizeof(script), "%s", wifi_coprocessor_.experiment);
  wifi_coprocessor_.experiment[0] = '\0';
  if (wifi_.hosted_bridge_initialized.load() ||
      wifi_.init_task_running.load() || !driver_.IsXl9535Ready()) {
    SetWifiCoprocessorStage(
        WifiCoprocessorUpdateStage::kFailed, "Wi-Fi is on; switch it off");
    return;
  }

  CoprocessorSdio sdio(MakeSdioConfig(&driver_));
  EspAtSdioLink link(&sdio);
  char version[sizeof(wifi_coprocessor_.detail)] = {};
  const WifiCoprocessorFirmware firmware =
      Identify(&sdio, &link, version, sizeof(version));
  if (firmware != WifiCoprocessorFirmware::kEspAt) {
    sdio.Close();
    driver_.SetEsp32c6PowerEnabled(false);
    SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kFailed,
        "The Wi-Fi chip does not run ESP-AT");
    return;
  }

  for (char* step = std::strtok(script, ";"); step != nullptr;
       step = std::strtok(nullptr, ";")) {
    if (std::strcmp(step, "!wait") == 0) {
      vTaskDelay(pdMS_TO_TICKS(kDownloadBootMs));
      continue;
    }
    if (std::strcmp(step, "!connect") == 0) {
      CoprocessorLoaderPort port = {};
      port.port.ops = CoprocessorLoaderPortOps();
      port.sdio = &sdio;
      port.max_freq_khz = kSdioFrequencyKhz;
      port.download_mode_entered = true;
      esp_loader_t loader;
      esp_loader_connect_args_t arguments = ESP_LOADER_CONNECT_DEFAULT();
      esp_loader_error_t result = esp_loader_init_sdio(&loader, &port.port);
      if (result == ESP_LOADER_SUCCESS) {
        result = esp_loader_connect(&loader, &arguments);
      }
      uint32_t flash_size = 0;
      if (result == ESP_LOADER_SUCCESS) {
        result = esp_loader_flash_detect_size(&loader, &flash_size);
      }
      LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
          "Wi-Fi co-processor: !connect -> %d, flash %lu bytes\n",
          static_cast<int>(result), static_cast<unsigned long>(flash_size));
      continue;
    }
    std::string reply;
    const EspAtSdioLink::Result result =
        link.Command(step, kCommandTimeoutMs, &reply);
    for (char& character : reply) {
      if (character == '\r' || character == '\n') {
        character = '|';
      }
    }
    LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
        "Wi-Fi co-processor: %s -> %d [%s]\n", step, static_cast<int>(result),
        reply.c_str());
  }

  sdio.Reset(kResetHoldMs);
  sdio.Close();
  driver_.SetEsp32c6PowerEnabled(false);
  SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kDone, version);
}

void TDisplayP4Device::RunWifiCoprocessorTask() {
  if (wifi_coprocessor_.experiment[0] != '\0') {
    RunWifiCoprocessorExperiment();
    return;
  }
  const WifiCoprocessorUpdateMode mode = wifi_coprocessor_.mode.load();
  const auto fail = [this](const char* reason) {
    SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kFailed, reason);
  };

  PackedImage image;
  if (mode == WifiCoprocessorUpdateMode::kInstall && !ReadPackedImage(&image)) {
    fail("This build carries no Wi-Fi chip firmware");
    return;
  }

  // ESP-Hosted and this task cannot share the bus.
  if (wifi_.hosted_bridge_initialized.load() ||
      wifi_.driver_initialized.load() || wifi_.init_task_running.load()) {
    SetWifiEnabled(false);
  }
  uint32_t waited_ms = 0;
  while (wifi_.hosted_bridge_initialized.load() ||
         wifi_.init_task_running.load()) {
    if (waited_ms >= kWifiStopTimeoutMs) {
      fail("Wi-Fi is busy; switch it off and try again");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(kPollMs));
    waited_ms += kPollMs;
  }
  if (!driver_.IsXl9535Ready()) {
    fail("The board's IO expander is not ready");
    return;
  }

  CoprocessorSdio sdio(MakeSdioConfig(&driver_));
  EspAtSdioLink link(&sdio);
  char version[sizeof(wifi_coprocessor_.detail)] = {};
  const WifiCoprocessorFirmware firmware =
      Identify(&sdio, &link, version, sizeof(version));
  if (firmware != WifiCoprocessorFirmware::kSilent) {
    wifi_coprocessor_.firmware.store(firmware);
  }
  // Whatever happens next, the chip is left switched off; Wi-Fi powers it up
  // again when it starts.
  const auto power_down = [this, &sdio]() {
    sdio.Close();
    driver_.SetEsp32c6PowerEnabled(false);
  };

  if (mode == WifiCoprocessorUpdateMode::kIdentify) {
    power_down();
    if (firmware == WifiCoprocessorFirmware::kSilent) {
      fail("The Wi-Fi chip does not answer");
    } else {
      SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kDone,
          firmware == WifiCoprocessorFirmware::kEspAt
              ? version
              : "ESP-Hosted (no update needed)");
    }
    return;
  }
  // "Not ESP-AT" is only taken for ESP-Hosted when ESP-Hosted has been seen
  // working on this chip; otherwise it may just as well be an image that was
  // cut off half-way, and whoever asked for an install gets one.
  if (firmware == WifiCoprocessorFirmware::kHosted && ReadHostedVerified()) {
    power_down();
    SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kDone,
        "ESP-Hosted (no update needed)");
    return;
  }

  // From here on it is either ESP-AT, or a chip with nothing usable on it
  // that only the BOOT button can bring into download mode.
  CoprocessorLoaderPort port = {};
  port.port.ops = CoprocessorLoaderPortOps();
  port.sdio = &sdio;
  port.max_freq_khz = kSdioFrequencyKhz;
  esp_loader_t loader;
  if (!sdio.Open() ||
      esp_loader_init_sdio(&loader, &port.port) != ESP_LOADER_SUCCESS) {
    power_down();
    fail("Could not open the SDIO bus to the Wi-Fi chip");
    return;
  }

  esp_loader_error_t result = ESP_LOADER_ERROR_FAIL;
  WifiCoprocessorFirmware answering = firmware;
  for (int attempt = 0; attempt < kSoftwareEntryAttempts &&
                        answering == WifiCoprocessorFirmware::kEspAt &&
                        result != ESP_LOADER_SUCCESS;
       ++attempt) {
    if (attempt > 0) {
      // Start over from a freshly reset chip.
      answering = Identify(&sdio, &link, version, sizeof(version));
      if (answering != WifiCoprocessorFirmware::kEspAt) {
        break;
      }
    }
    SetWifiCoprocessorStage(
        WifiCoprocessorUpdateStage::kEnteringDownloadMode, version);
    if (!EnterDownloadModeThroughAt(&link)) {
      continue;
    }
    SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kConnecting, nullptr);
    port.download_mode_entered = true;
    esp_loader_connect_args_t arguments = ESP_LOADER_CONNECT_DEFAULT();
    result = esp_loader_connect(&loader, &arguments);
    if (result != ESP_LOADER_SUCCESS) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Wi-Fi co-processor: download mode not reached (error %d)\n",
          static_cast<int>(result));
    }
  }
  if (result != ESP_LOADER_SUCCESS) {
    SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kNeedsBootButton,
        "Hold the Wi-Fi chip's BOOT button, then continue");
    waited_ms = 0;
    while (!wifi_coprocessor_.boot_button_confirmed.load()) {
      if (waited_ms >= kBootButtonTimeoutMs) {
        power_down();
        fail("Could not put the Wi-Fi chip into download mode");
        return;
      }
      vTaskDelay(pdMS_TO_TICKS(kPollMs));
      waited_ms += kPollMs;
    }
    SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kConnecting, nullptr);
    // The port resets the chip while the button holds the strap low.
    port.download_mode_entered = false;
    esp_loader_connect_args_t arguments = ESP_LOADER_CONNECT_DEFAULT();
    result = esp_loader_connect(&loader, &arguments);
    if (result != ESP_LOADER_SUCCESS) {
      power_down();
      fail("The Wi-Fi chip did not enter download mode");
      return;
    }
  }

  uint32_t flash_size = 0;
  if (esp_loader_flash_detect_size(&loader, &flash_size) !=
      ESP_LOADER_SUCCESS) {
    esp_loader_reset_target(&loader);
    power_down();
    fail("Could not read the Wi-Fi chip's flash size");
    return;
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "Wi-Fi co-processor: download mode reached, flash %lu bytes\n",
      static_cast<unsigned long>(flash_size));

  if (mode == WifiCoprocessorUpdateMode::kRehearse) {
    esp_loader_reset_target(&loader);
    power_down();
    char summary[sizeof(wifi_coprocessor_.detail)];
    std::snprintf(summary, sizeof(summary),
        "Rehearsal passed: download mode reached, flash %lu MB",
        static_cast<unsigned long>(flash_size / (1024 * 1024)));
    SetWifiCoprocessorStage(WifiCoprocessorUpdateStage::kDone, summary);
    return;
  }
  if (flash_size < image.image_size) {
    esp_loader_reset_target(&loader);
    power_down();
    fail("The Wi-Fi chip's flash is too small for the firmware");
    return;
  }

  SetWifiCoprocessorStage(
      WifiCoprocessorUpdateStage::kWriting, "Writing Wi-Fi chip firmware");
  // The library wants a writable block, and the image lives in flash.
  std::unique_ptr<uint8_t[]> block(new (std::nothrow) uint8_t[kFlashBlockBytes]);
  if (block == nullptr) {
    esp_loader_reset_target(&loader);
    power_down();
    fail("Out of memory");
    return;
  }
  esp_loader_flash_deflate_cfg_t flash = {};
  flash.offset = 0;
  flash.image_size = image.image_size;
  flash.compressed_size = image.stream_size;
  flash.block_size = kFlashBlockBytes;
  result = esp_loader_flash_deflate_start(&loader, &flash);
  uint32_t written = 0;
  while (result == ESP_LOADER_SUCCESS && written < image.stream_size) {
    const uint32_t size =
        std::min<uint32_t>(kFlashBlockBytes, image.stream_size - written);
    std::memcpy(block.get(), image.stream + written, size);
    result = esp_loader_flash_deflate_write(&loader, &flash, block.get(), size);
    written += size;
    const uint8_t percent = static_cast<uint8_t>(
        static_cast<uint64_t>(written) * 100 / image.stream_size);
    if (percent / 10 != wifi_coprocessor_.progress_percent.load() / 10) {
      LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
          "Wi-Fi co-processor: written %u %%\n", percent);
    }
    wifi_coprocessor_.progress_percent.store(percent);
  }
  if (result == ESP_LOADER_SUCCESS) {
    result = esp_loader_flash_deflate_finish(&loader, &flash);
  }
  if (result == ESP_LOADER_SUCCESS) {
    SetWifiCoprocessorStage(
        WifiCoprocessorUpdateStage::kVerifying, "Checking what was written");
    result = esp_loader_flash_verify_known_md5(&loader, 0, image.image_size,
        reinterpret_cast<const uint8_t*>(image.md5));
  }
  esp_loader_reset_target(&loader);
  power_down();

  if (result != ESP_LOADER_SUCCESS) {
    // The chip may hold a half-written image now. Nothing is lost for good:
    // its ROM still accepts firmware with the BOOT button held.
    WriteHostedVerified(false);
    wifi_coprocessor_.firmware.store(WifiCoprocessorFirmware::kUnknown);
    char reason[sizeof(wifi_coprocessor_.detail)];
    std::snprintf(reason, sizeof(reason),
        "Writing the Wi-Fi chip firmware failed (error %d)",
        static_cast<int>(result));
    fail(reason);
    return;
  }

  wifi_coprocessor_.firmware.store(WifiCoprocessorFirmware::kHosted);
  app::SetHardwareDiagnostic(app::HardwareDiagnostic::kWifi, "");
  SetWifiCoprocessorStage(
      WifiCoprocessorUpdateStage::kDone, "Wi-Fi chip firmware updated");
}

bool TDisplayP4Device::EnsureWifiCoprocessorIsHosted() {
  WifiCoprocessorFirmware firmware = wifi_coprocessor_.firmware.load();
  if (firmware == WifiCoprocessorFirmware::kHosted) {
    return true;
  }
  if (firmware == WifiCoprocessorFirmware::kEspAt ||
      wifi_coprocessor_.task_running.load()) {
    return false;
  }
  if (ReadHostedVerified()) {
    wifi_coprocessor_.firmware.store(WifiCoprocessorFirmware::kHosted);
    return true;
  }

  CoprocessorSdio sdio(MakeSdioConfig(&driver_));
  EspAtSdioLink link(&sdio);
  char version[sizeof(wifi_coprocessor_.detail)] = {};
  firmware = Identify(&sdio, &link, version, sizeof(version));
  sdio.Close();
  if (firmware == WifiCoprocessorFirmware::kSilent) {
    // Not remembered: the next attempt looks again.
    SetWifiCoprocessorStage(
        wifi_coprocessor_.stage.load(), "The Wi-Fi chip does not answer");
    return false;
  }
  wifi_coprocessor_.firmware.store(firmware);
  if (firmware == WifiCoprocessorFirmware::kEspAt) {
    SetWifiCoprocessorStage(wifi_coprocessor_.stage.load(), version);
    return false;
  }
  return true;
}

void TDisplayP4Device::MarkWifiCoprocessorVerified() {
  if (wifi_coprocessor_.firmware.exchange(WifiCoprocessorFirmware::kHosted) !=
          WifiCoprocessorFirmware::kHosted ||
      !ReadHostedVerified()) {
    WriteHostedVerified(true);
  }
}

}  // namespace lilygo_box::hal
