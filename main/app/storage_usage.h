/*
 * @Description: What takes how much room on the SD card and in the phone
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>

#include "hal/providers/storage_provider.h"

namespace lilygo_box::app {

struct StorageUsage {
  bool card_present = false;
  bool measuring = false;
  uint64_t card_total = 0;
  uint64_t card_free = 0;
  // Top-level folders of the card.
  uint64_t maps = 0;
  uint64_t wikipedia = 0;
  uint64_t nature = 0;
  uint64_t translator = 0;
  uint64_t music = 0;
  uint64_t photos = 0;
  uint64_t recently_deleted = 0;
  // The phone's own memory.
  uint64_t flash_total = 0;
  uint64_t system = 0;      // the running firmware
  uint64_t first_aid = 0;   // cabinet and people
  uint64_t internal_total = 0;
  uint64_t internal_used = 0;
};

/**
 * @brief Measures in the background; a walk of the card takes a few seconds.
 *        Does nothing while a measurement runs.
 */
void StartStorageUsageScan(hal::StorageProvider* storage);

/**
 * @brief The figures so far; they grow while `measuring` is true.
 */
StorageUsage GetStorageUsage();

}  // namespace lilygo_box::app
