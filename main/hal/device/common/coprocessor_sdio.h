/*
 * @Description: SDIO access to the wireless co-processor outside ESP-Hosted
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "esp_loader_io.h"
#include "sdmmc_cmd.h"

namespace lilygo_box::hal {

// SurvivalOS: early T-Display-P4 units carry ESP-AT on the ESP32-C6, which
// ESP-Hosted cannot talk to. These helpers reach the co-processor directly:
// as an ESP-AT text link (to identify it and send it into download mode) and
// as an esp-serial-flasher port (to write the ESP-Hosted firmware over SDIO).

struct CoprocessorSdioConfig {
  // SDMMC host slot the co-processor is wired to.
  int slot = 1;
  int clk = -1;
  int cmd = -1;
  int d0 = -1;
  int d1 = -1;
  int d2 = -1;
  int d3 = -1;
  // Drives the co-processor's EN line: true lets it run, false holds it in
  // reset.
  bool (*set_enabled)(void* context, bool enabled) = nullptr;
  void* context = nullptr;
};

/**
 * @brief One slot of the shared SDMMC host.
 *
 * The SD card lives on the other slot of the same host, so this only ever
 * claims and releases its own slot.
 */
class CoprocessorSdio {
 public:
  explicit CoprocessorSdio(const CoprocessorSdioConfig& config)
      : config_(config) {}
  ~CoprocessorSdio() { Close(); }
  CoprocessorSdio(const CoprocessorSdio&) = delete;
  CoprocessorSdio& operator=(const CoprocessorSdio&) = delete;

  /** @brief Claims the slot. Safe to call when it is already open. */
  bool Open();

  /** @brief Releases the slot and the card's bookkeeping. */
  void Close();

  /**
   * @brief Runs the SDIO card initialisation against the co-processor.
   * @param max_freq_khz Bus clock limit.
   */
  bool InitCard(uint32_t max_freq_khz);

  /**
   * @brief Pulses the EN line.
   * @param low_ms How long reset is held.
   */
  bool Reset(uint32_t low_ms);

  sdmmc_card_t* card() { return &card_; }

 private:
  CoprocessorSdioConfig config_;
  sdmmc_card_t card_ = {};
  bool slot_open_ = false;
};

/**
 * @brief Text link to ESP-AT over the ESP SDIO slave protocol.
 */
class EspAtSdioLink {
 public:
  enum class Result : uint8_t { kOk, kError, kTimeout, kBusError };

  explicit EspAtSdioLink(CoprocessorSdio* sdio) : sdio_(sdio) {}

  /** @brief Enables SDIO function 1 and sets the block sizes. */
  bool Start();

  /**
   * @brief Appends whatever the firmware has sent since the last call.
   * @return false on a bus error; true also when nothing was pending.
   */
  bool Receive(std::string* text);

  /** @brief Sends raw text. */
  bool Send(const char* data, size_t size);

  /**
   * @brief Collects incoming text until a marker shows up.
   * @param marker Text to wait for.
   * @param collected Receives everything read, marker included.
   */
  Result WaitFor(
      const char* marker, uint32_t timeout_ms, std::string* collected);

  /**
   * @brief Sends one AT command and collects the reply up to OK or ERROR.
   * @param command Command without the line ending.
   * @param reply Receives the reply text; may be nullptr.
   */
  Result Command(
      const char* command, uint32_t timeout_ms, std::string* reply);

 private:
  CoprocessorSdio* sdio_;
  // Bytes the co-processor has sent so far, as counted by its own length
  // register (which wraps at 2^20).
  uint32_t received_total_ = 0;
};

/**
 * @brief esp-serial-flasher port bound to a CoprocessorSdio.
 *
 * `port` must stay the first member: the library hands back a pointer to it.
 */
struct CoprocessorLoaderPort {
  esp_loader_port_t port;
  CoprocessorSdio* sdio = nullptr;
  uint32_t max_freq_khz = 0;
  // True when the co-processor is already sitting in its ROM download mode
  // and must not be reset again before connecting.
  bool download_mode_entered = false;
  int64_t deadline_us = 0;
};

/** @brief Port operations for CoprocessorLoaderPort. */
const esp_loader_port_ops_t* CoprocessorLoaderPortOps();

}  // namespace lilygo_box::hal
