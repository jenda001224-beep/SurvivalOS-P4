/*
 * @Description: SDIO access to the wireless co-processor outside ESP-Hosted
 * @License: GPL 3.0
 */
#include "hal/device/common/coprocessor_sdio.h"

#include <unistd.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "base/logger.h"
#include "driver/sdmmc_host.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace lilygo_box::hal {
namespace {

// SDIO common registers (function 0).
constexpr uint32_t kCccrFunctionEnable = 0x02;
constexpr uint32_t kCccrFunctionReady = 0x03;
constexpr uint32_t kCccrBlockSizeLow = 0x10;
constexpr uint32_t kCccrBlockSizeHigh = 0x11;
constexpr uint32_t kFunction1BlockSizeLow = 0x110;
constexpr uint32_t kFunction1BlockSizeHigh = 0x111;
constexpr uint8_t kFunction1Bit = 0x02;

// ESP SDIO slave registers (function 1), the same ones ESP-AT's own host
// example and ESP-IDF's serial slave link use.
constexpr uint32_t kSlaveTokenRdata = 0x44;
constexpr uint32_t kSlaveInterruptRaw = 0x50;
constexpr uint32_t kSlavePacketLength = 0x60;
constexpr uint32_t kSlaveInterruptClear = 0xD4;
constexpr uint32_t kSlavePacketEnd = 0x1F800;
constexpr uint32_t kSlaveNewPacketInterrupt = 1U << 23;
constexpr uint32_t kSlaveLengthMask = 0xFFFFF;
constexpr uint32_t kSlaveTokenShift = 16;
constexpr uint32_t kSlaveTokenMask = 0xFFF;

constexpr size_t kSdioChunkBytes = 512;
constexpr uint32_t kPollIntervalMs = 10;
constexpr uint32_t kFunctionReadyTimeoutMs = 1000;
constexpr uint32_t kSendTokenTimeoutMs = 1000;
constexpr uint32_t kBootSettleMs = 50;
constexpr uint32_t kResetHoldMs = 100;

size_t AlignTo4(size_t size) { return (size + 3) & ~static_cast<size_t>(3); }

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void DelayMs(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

CoprocessorLoaderPort* PortFrom(esp_loader_port_t* port) {
  return container_of(port, CoprocessorLoaderPort, port);
}

esp_loader_error_t ToLoaderError(esp_err_t result) {
  if (result == ESP_OK) {
    return ESP_LOADER_SUCCESS;
  }
  return result == ESP_ERR_TIMEOUT ? ESP_LOADER_ERROR_TIMEOUT
                                   : ESP_LOADER_ERROR_FAIL;
}

esp_loader_error_t PortInit(esp_loader_port_t* port) {
  // The slot is opened by whoever owns the CoprocessorSdio.
  return PortFrom(port)->sdio != nullptr ? ESP_LOADER_SUCCESS
                                         : ESP_LOADER_ERROR_FAIL;
}

void PortEnterBootloader(esp_loader_port_t* port) {
  CoprocessorLoaderPort* self = PortFrom(port);
  if (!self->download_mode_entered) {
    // The BOOT strap is a button on this board: whoever holds it decides the
    // mode, this only provides the reset edge that samples it.
    self->sdio->Reset(kResetHoldMs);
  }
  DelayMs(kBootSettleMs);
}

void PortResetTarget(esp_loader_port_t* port) {
  CoprocessorLoaderPort* self = PortFrom(port);
  self->download_mode_entered = false;
  self->sdio->Reset(kResetHoldMs);
}

void PortStartTimer(esp_loader_port_t* port, uint32_t ms) {
  PortFrom(port)->deadline_us =
      esp_timer_get_time() + static_cast<int64_t>(ms) * 1000;
}

uint32_t PortRemainingTime(esp_loader_port_t* port) {
  const int64_t remaining_ms =
      (PortFrom(port)->deadline_us - esp_timer_get_time()) / 1000;
  return remaining_ms > 0 ? static_cast<uint32_t>(remaining_ms) : 0;
}

void PortDelayMs(esp_loader_port_t* port, uint32_t ms) {
  (void)port;
  usleep(ms * 1000);
}

void PortLog(esp_loader_port_t* port, esp_loader_log_level_t level,
    const char* format, va_list arguments) {
  (void)port;
  char line[160];
  vsnprintf(line, sizeof(line), format, arguments);
  const LogLevel mapped =
      level <= ESP_LOADER_LOG_LEVEL_ERROR  ? LogLevel::kError
      : level == ESP_LOADER_LOG_LEVEL_WARN ? LogLevel::kWarning
                                           : LogLevel::kInfo;
  LogMessage(mapped, __FILE__, __LINE__, "co-processor flasher: %s\n", line);
}

esp_loader_error_t PortSdioWrite(esp_loader_port_t* port, uint32_t function,
    uint32_t address, const uint8_t* data, uint16_t size, uint32_t timeout) {
  (void)timeout;
  if (data == nullptr) {
    return ESP_LOADER_ERROR_INVALID_PARAM;
  }
  return ToLoaderError(sdmmc_io_write_bytes(
      PortFrom(port)->sdio->card(), function, address, data, size));
}

esp_loader_error_t PortSdioRead(esp_loader_port_t* port, uint32_t function,
    uint32_t address, uint8_t* data, uint16_t size, uint32_t timeout) {
  (void)timeout;
  if (data == nullptr) {
    return ESP_LOADER_ERROR_INVALID_PARAM;
  }
  return ToLoaderError(sdmmc_io_read_bytes(
      PortFrom(port)->sdio->card(), function, address, data, size));
}

esp_loader_error_t PortSdioCardInit(esp_loader_port_t* port) {
  CoprocessorLoaderPort* self = PortFrom(port);
  return self->sdio->InitCard(self->max_freq_khz) ? ESP_LOADER_SUCCESS
                                                  : ESP_LOADER_ERROR_FAIL;
}

const esp_loader_port_ops_t kLoaderPortOps = {
    .init = PortInit,
    .deinit = nullptr,
    .enter_bootloader = PortEnterBootloader,
    .reset_target = PortResetTarget,
    .start_timer = PortStartTimer,
    .remaining_time = PortRemainingTime,
    .delay_ms = PortDelayMs,
    .log = PortLog,
    .log_hex = nullptr,
    .change_transmission_rate = nullptr,
    .write = nullptr,
    .read = nullptr,
    .spi_set_cs = nullptr,
    .sdio_write = PortSdioWrite,
    .sdio_read = PortSdioRead,
    .sdio_card_init = PortSdioCardInit,
};

}  // namespace

bool CoprocessorSdio::Open() {
  if (slot_open_) {
    return true;
  }
  // Returns ESP_OK without touching anything when the SD card slot has
  // already brought the host up.
  esp_err_t result = sdmmc_host_init();
  if (result != ESP_OK) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Co-processor SDIO: host init failed (%#X)\n",
        static_cast<unsigned>(result));
    return false;
  }

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.width = 4;
  slot.clk = static_cast<gpio_num_t>(config_.clk);
  slot.cmd = static_cast<gpio_num_t>(config_.cmd);
  slot.d0 = static_cast<gpio_num_t>(config_.d0);
  slot.d1 = static_cast<gpio_num_t>(config_.d1);
  slot.d2 = static_cast<gpio_num_t>(config_.d2);
  slot.d3 = static_cast<gpio_num_t>(config_.d3);
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  result = sdmmc_host_init_slot(config_.slot, &slot);
  if (result != ESP_OK) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Co-processor SDIO: slot %d init failed (%#X)\n", config_.slot,
        static_cast<unsigned>(result));
    return false;
  }
  slot_open_ = true;
  return true;
}

void CoprocessorSdio::Close() {
  // sdmmc_card_init() allocates this buffer and offers no call to free it.
  free(card_.host.dma_aligned_buffer);
  card_ = {};
  if (slot_open_) {
    // Only this slot: the SD card on the other one stays mounted.
    sdmmc_host_deinit_slot(config_.slot);
    slot_open_ = false;
  }
}

bool CoprocessorSdio::InitCard(uint32_t max_freq_khz) {
  if (!slot_open_) {
    return false;
  }
  free(card_.host.dma_aligned_buffer);
  card_ = {};

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = config_.slot;
  host.max_freq_khz = static_cast<int>(max_freq_khz);
  // Lets transfers use buffers of any alignment and length.
  host.flags |= SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
  return sdmmc_card_init(&host, &card_) == ESP_OK;
}

bool CoprocessorSdio::Reset(uint32_t low_ms) {
  if (config_.set_enabled == nullptr) {
    return false;
  }
  bool result = config_.set_enabled(config_.context, false);
  DelayMs(low_ms);
  result &= config_.set_enabled(config_.context, true);
  return result;
}

bool EspAtSdioLink::Start() {
  received_total_ = 0;
  sdmmc_card_t* card = sdio_->card();

  uint8_t enabled = 0;
  if (sdmmc_io_read_byte(card, 0, kCccrFunctionEnable, &enabled) != ESP_OK ||
      sdmmc_io_write_byte(card, 0, kCccrFunctionEnable,
          enabled | kFunction1Bit, nullptr) != ESP_OK) {
    return false;
  }
  const uint32_t started_ms = NowMs();
  uint8_t ready = 0;
  while ((ready & kFunction1Bit) == 0) {
    if (sdmmc_io_read_byte(card, 0, kCccrFunctionReady, &ready) != ESP_OK ||
        NowMs() - started_ms > kFunctionReadyTimeoutMs) {
      return false;
    }
  }

  // 512-byte blocks for the card as a whole and for function 1.
  return sdmmc_io_write_byte(card, 0, kCccrBlockSizeLow, 0x00, nullptr) ==
             ESP_OK &&
         sdmmc_io_write_byte(card, 0, kCccrBlockSizeHigh, 0x02, nullptr) ==
             ESP_OK &&
         sdmmc_io_write_byte(card, 0, kFunction1BlockSizeLow, 0x00, nullptr) ==
             ESP_OK &&
         sdmmc_io_write_byte(card, 0, kFunction1BlockSizeHigh, 0x02,
             nullptr) == ESP_OK;
}

bool EspAtSdioLink::Receive(std::string* text) {
  sdmmc_card_t* card = sdio_->card();

  uint32_t interrupts = 0;
  if (sdmmc_io_read_bytes(card, 1, kSlaveInterruptRaw, &interrupts,
          sizeof(interrupts)) != ESP_OK) {
    return false;
  }
  if ((interrupts & kSlaveNewPacketInterrupt) != 0) {
    uint32_t clear = kSlaveNewPacketInterrupt;
    if (sdmmc_io_write_bytes(card, 1, kSlaveInterruptClear, &clear,
            sizeof(clear)) != ESP_OK) {
      return false;
    }
  }

  uint32_t length_register = 0;
  if (sdmmc_io_read_bytes(card, 1, kSlavePacketLength, &length_register,
          sizeof(length_register)) != ESP_OK) {
    return false;
  }
  // The register counts every byte sent since reset and wraps.
  uint32_t pending =
      ((length_register & kSlaveLengthMask) - received_total_) &
      kSlaveLengthMask;
  uint8_t chunk[kSdioChunkBytes];
  while (pending > 0) {
    const size_t wanted = std::min<size_t>(pending, sizeof(chunk));
    // Data sits right below the end address; a short tail is read padded to
    // four bytes.
    if (sdmmc_io_read_bytes(card, 1, kSlavePacketEnd - pending, chunk,
            AlignTo4(wanted)) != ESP_OK) {
      return false;
    }
    if (text != nullptr) {
      text->append(reinterpret_cast<const char*>(chunk), wanted);
    }
    pending -= wanted;
    received_total_ = (received_total_ + wanted) & kSlaveLengthMask;
  }
  return true;
}

bool EspAtSdioLink::Send(const char* data, size_t size) {
  if (data == nullptr || size == 0 || size > kSdioChunkBytes) {
    return false;
  }
  sdmmc_card_t* card = sdio_->card();

  // Wait until the firmware has a receive buffer on offer.
  const uint32_t started_ms = NowMs();
  while (true) {
    uint32_t token = 0;
    if (sdmmc_io_read_bytes(card, 1, kSlaveTokenRdata, &token,
            sizeof(token)) != ESP_OK) {
      return false;
    }
    if (((token >> kSlaveTokenShift) & kSlaveTokenMask) != 0) {
      break;
    }
    if (NowMs() - started_ms > kSendTokenTimeoutMs) {
      return false;
    }
    DelayMs(kPollIntervalMs);
  }

  uint8_t padded[kSdioChunkBytes] = {};
  std::memcpy(padded, data, size);
  return sdmmc_io_write_bytes(
             card, 1, kSlavePacketEnd - size, padded, AlignTo4(size)) == ESP_OK;
}

EspAtSdioLink::Result EspAtSdioLink::WaitFor(
    const char* marker, uint32_t timeout_ms, std::string* collected) {
  std::string local;
  std::string* text = collected != nullptr ? collected : &local;
  const uint32_t started_ms = NowMs();
  while (true) {
    if (!Receive(text)) {
      return Result::kBusError;
    }
    if (text->find(marker) != std::string::npos) {
      return Result::kOk;
    }
    if (NowMs() - started_ms > timeout_ms) {
      return Result::kTimeout;
    }
    DelayMs(kPollIntervalMs);
  }
}

EspAtSdioLink::Result EspAtSdioLink::Command(
    const char* command, uint32_t timeout_ms, std::string* reply) {
  std::string line(command);
  line += "\r\n";
  if (!Send(line.data(), line.size())) {
    return Result::kBusError;
  }

  std::string local;
  std::string* text = reply != nullptr ? reply : &local;
  text->clear();
  const uint32_t started_ms = NowMs();
  while (true) {
    if (!Receive(text)) {
      return Result::kBusError;
    }
    if (text->find("\r\nOK\r\n") != std::string::npos) {
      return Result::kOk;
    }
    if (text->find("\r\nERROR\r\n") != std::string::npos) {
      return Result::kError;
    }
    if (NowMs() - started_ms > timeout_ms) {
      return Result::kTimeout;
    }
    DelayMs(kPollIntervalMs);
  }
}

const esp_loader_port_ops_t* CoprocessorLoaderPortOps() {
  return &kLoaderPortOps;
}

}  // namespace lilygo_box::hal
