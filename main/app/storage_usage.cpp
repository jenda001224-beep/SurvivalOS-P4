/*
 * @Description: What takes how much room on the SD card and in the phone
 * @License: GPL 3.0
 *
 * The folders are walked through FatFs rather than stat(): newlib's st_size is
 * 32 bits and a single exFAT map or Wikipedia file goes far past that.
 */
#include "app/storage_usage.h"

#include <strings.h>
#include <sys/stat.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "app/photo_trash.h"
#include "app/storage/littlefs_storage.h"
#include "esp_flash.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace lilygo_box::app {
namespace {

constexpr int kMaxDepth = 6;
constexpr size_t kMaxPath = 280;

std::mutex g_mutex;
StorageUsage g_usage;
std::atomic<bool> g_running{false};
hal::StorageProvider* g_storage = nullptr;

template <typename Change>
void Update(Change change) {
  std::lock_guard<std::mutex> guard(g_mutex);
  change(&g_usage);
}

uint64_t FolderBytes(char* path, size_t length, int depth, const char* skip) {
  FF_DIR folder;
  if (depth > kMaxDepth || f_opendir(&folder, path) != FR_OK) {
    return 0;
  }
  uint64_t bytes = 0;
  FILINFO info;
  while (f_readdir(&folder, &info) == FR_OK && info.fname[0] != '\0') {
    if (info.fattrib & AM_DIR) {
      if (skip != nullptr && strcasecmp(info.fname, skip) == 0) {
        continue;
      }
      const int added = std::snprintf(path + length, kMaxPath - length, "/%s", info.fname);
      if (added > 0 && length + added < kMaxPath) {
        bytes += FolderBytes(path, length + added, depth + 1, nullptr);
      }
      path[length] = '\0';
    } else {
      bytes += info.fsize;
    }
  }
  f_closedir(&folder);
  return bytes;
}

// The drive FatFs knows the card under: the first one that opens.
bool FindDrive(char* drive, size_t size) {
  for (int number = 0; number < FF_VOLUMES; ++number) {
    std::snprintf(drive, size, "%d:", number);
    char root[16];
    std::snprintf(root, sizeof(root), "%d:/", number);
    FF_DIR folder;
    if (f_opendir(&folder, root) == FR_OK) {
      f_closedir(&folder);
      return true;
    }
  }
  return false;
}

void MeasureCard() {
  hal::StorageProvider* storage = g_storage;
  if (storage == nullptr || !storage->EnsureSdCardMounted()) {
    Update([](StorageUsage* usage) {
      usage->card_present = false;
      usage->card_total = usage->card_free = 0;
    });
    return;
  }
  uint64_t total = 0;
  uint64_t free_bytes = 0;
  if (esp_vfs_fat_info(storage->SdCardBasePath(), &total, &free_bytes) != ESP_OK) {
    return;
  }
  Update([&](StorageUsage* usage) {
    usage->card_present = true;
    usage->card_total = total;
    usage->card_free = free_bytes;
  });
  char drive[8];
  if (!FindDrive(drive, sizeof(drive))) {
    return;
  }
  // The folder that files sort into by themselves; a name in another case is
  // the same folder on FAT.
  const struct {
    const char* folder;
    uint64_t StorageUsage::*field;
  } kFolders[] = {
      {"maps", &StorageUsage::maps},
      {"wiki", &StorageUsage::wikipedia},
      {"nature", &StorageUsage::nature},
      {"translator", &StorageUsage::translator},
      {"Music", &StorageUsage::music},
  };
  static char path[kMaxPath];
  for (const auto& entry : kFolders) {
    const int length = std::snprintf(path, sizeof(path), "%s/%s", drive, entry.folder);
    const uint64_t bytes = FolderBytes(path, length, 0, nullptr);
    Update([&](StorageUsage* usage) { usage->*(entry.field) = bytes; });
  }
  int length = std::snprintf(path, sizeof(path), "%s/DCIM", drive);
  const uint64_t photos = FolderBytes(path, length, 0, kPhotoTrashDirectory);
  Update([&](StorageUsage* usage) { usage->photos = photos; });
  length = std::snprintf(path, sizeof(path), "%s/DCIM/%s", drive, kPhotoTrashDirectory);
  const uint64_t deleted = FolderBytes(path, length, 0, nullptr);
  Update([&](StorageUsage* usage) { usage->recently_deleted = deleted; });
}

void MeasurePhone() {
  uint32_t flash = 0;
  esp_flash_get_size(nullptr, &flash);
  uint64_t system = 0;
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (running != nullptr) {
    esp_image_metadata_t image = {};
    const esp_partition_pos_t position = {running->address, running->size};
    if (esp_image_get_metadata(&position, &image) == ESP_OK) {
      system = image.image_len;
    }
  }
  size_t total = 0;
  size_t used = 0;
  GetLittleFsStorageInfo(&total, &used);
  uint64_t first_aid = 0;
  const char* base = LittleFsStorageBasePath();
  if (base != nullptr) {
    char path[64];
    std::snprintf(path, sizeof(path), "%s/firstaid.txt", base);
    struct stat information {};
    if (stat(path, &information) == 0) {
      first_aid = static_cast<uint64_t>(information.st_size);
    }
  }
  Update([&](StorageUsage* usage) {
    usage->flash_total = flash;
    usage->system = system;
    usage->first_aid = first_aid;
    usage->internal_total = total;
    usage->internal_used = used;
  });
}

void ScanTask(void* /*context*/) {
  MeasurePhone();
  MeasureCard();
  Update([](StorageUsage* usage) { usage->measuring = false; });
  g_running.store(false);
  vTaskDelete(nullptr);
}

}  // namespace

void StartStorageUsageScan(hal::StorageProvider* storage) {
  bool expected = false;
  if (!g_running.compare_exchange_strong(expected, true)) {
    return;
  }
  g_storage = storage;
  Update([](StorageUsage* usage) { usage->measuring = true; });
  if (xTaskCreate(ScanTask, "storage_usage", 10 * 1024, nullptr, 1, nullptr) != pdPASS) {
    Update([](StorageUsage* usage) { usage->measuring = false; });
    g_running.store(false);
  }
}

StorageUsage GetStorageUsage() {
  std::lock_guard<std::mutex> guard(g_mutex);
  return g_usage;
}

}  // namespace lilygo_box::app
