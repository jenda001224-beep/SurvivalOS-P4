/*
 * @Description: T-Display-P4 SD 与 USB 存储实现
 * @Author: LILYGO_L
 * @Date: 2026-08-28 00:00:00
 * @LastEditTime: 2026-09-02 17:53:15
 * @License: GPL 3.0
 */
#include <sys/stat.h>

#include <cstdio>
#include <mutex>

#include "app/diagnostics/hardware_diagnostics.h"
#include "base/logger.h"
#include "hal/device/t_display_p4/device.h"

namespace lilygo_box::hal {
namespace device = lilygo_device_driver::t_display_p4::device;

namespace {

// SurvivalOS: the card is now also mounted in the background at boot, so two
// tasks can ask for it at the same moment; the driver's mount is not reentrant.
std::mutex g_sd_mount_mutex;

}  // namespace

bool TDisplayP4Device::EnsureSdCardMounted() {
  const std::lock_guard<std::mutex> lock(g_sd_mount_mutex);
  if (IsSdCardMounted()) {
    return true;
  }

  const uint32_t log_mark = app::LogCaptureMark();
  bool result = driver_.InitSdmmc(device::sd::kBasePath, SDMMC_FREQ_52M);
  if (!result) {
    // SurvivalOS: a large card that cannot hold a clean high-speed link on this
    // board fails here with CRC or timeout errors. Default speed (20 MHz) is
    // slower but far more forgiving, and a slow card beats no card. The driver
    // only reports success or failure, so this also runs when no card is
    // inserted; that costs one more short probe.
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "InitSdmmc failed at high speed; retrying at default speed\n");
    result = driver_.InitSdmmc(device::sd::kBasePath, SDMMC_FREQ_DEFAULT);
  }
  if (!result) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "InitSdmmc failed: no card, or the card is not FAT32/exFAT\n");
    // Keep what ESP-IDF said about it, so the reason can be read on screen.
    char reason[200] = {};
    app::CollectLogLinesSince(log_mark, reason, sizeof(reason));
    app::SetHardwareDiagnostic(app::HardwareDiagnostic::kSdCard,
        reason[0] != '\0' ? reason : "mount failed, no detail logged");
    return false;
  }
  app::SetHardwareDiagnostic(app::HardwareDiagnostic::kSdCard, "");
  return IsSdCardMounted();
}

bool TDisplayP4Device::UnmountSdCard() {
  const std::lock_guard<std::mutex> lock(g_sd_mount_mutex);
  return driver_.DeinitSdmmc();
}

bool TDisplayP4Device::IsSdCardMounted() const {
  if (!driver_.IsSdmmcReady()) {
    return false;
  }
  struct stat info = {};
  return stat(device::sd::kBasePath, &info) == 0 && S_ISDIR(info.st_mode);
}

const char* TDisplayP4Device::SdCardBasePath() const {
  return device::sd::kBasePath;
}

bool TDisplayP4Device::StartUsbStorage() {
  if (!driver_.SetUsbHostPowerEnabled(true)) {
    return false;
  }
  return usb_storage_manager_.Start();
}

bool TDisplayP4Device::StopUsbStorage() {
  // USB PHY 供电保持开启可避免 ESP32-P4 产生约 20 mA 的额外功耗。
  return usb_storage_manager_.Stop();
}

bool TDisplayP4Device::ReadUsbStorageSnapshot(
    UsbStorageSnapshot* snapshot) const {
  return usb_storage_manager_.ReadSnapshot(snapshot);
}

}  // namespace lilygo_box::hal
