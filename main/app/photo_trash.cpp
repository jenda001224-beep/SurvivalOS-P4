/*
 * @Description: Recently Deleted for the Photos app
 * @License: GPL 3.0
 */
#include "app/photo_trash.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "app/edition.h"
#include "app/storage/littlefs_storage.h"
#include "base/logger.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace lilygo_box::app {
namespace {

constexpr const char* kPhotoDirectoryName = "DCIM";
constexpr const char* kThumbnailDirectory = ".thumbs";
constexpr size_t kMaxPath = 192;
// Below this much free room the oldest deletions go.
constexpr uint64_t kCardLowBytes = 500ULL * 1024 * 1024;
constexpr uint64_t kInternalLowBytes = 512ULL * 1024;

std::atomic<bool> g_purge_running{false};
uint64_t g_low_bytes_for_test = 0;
hal::StorageProvider* g_purge_storage = nullptr;

void MoveThumbnail(const char* from_directory, const char* from_name,
    const char* to_directory, const char* to_name) {
  char from[kMaxPath + 80];
  char to[kMaxPath + 80];
  std::snprintf(from, sizeof(from), "%s/%s/%s.thm", from_directory, kThumbnailDirectory, from_name);
  struct stat information {};
  if (stat(from, &information) != 0) {
    return;  // never made, or the phone's own memory, which has no cache
  }
  std::snprintf(to, sizeof(to), "%s/%s", to_directory, kThumbnailDirectory);
  mkdir(to, 0775);
  std::snprintf(to, sizeof(to), "%s/%s/%s.thm", to_directory, kThumbnailDirectory, to_name);
  rename(from, to);
}

bool FreeBytes(hal::StorageProvider* storage, const char* base, bool on_card, uint64_t* free_bytes) {
  (void)storage;
  if (on_card) {
    uint64_t total = 0;
    return esp_vfs_fat_info(base, &total, free_bytes) == ESP_OK;
  }
  size_t total = 0;
  size_t used = 0;
  if (!GetLittleFsStorageInfo(&total, &used) || used > total) {
    return false;
  }
  *free_bytes = total - used;
  return true;
}

// The oldest entry: names start with the deletion time.
bool OldestTrashedPhoto(const char* trash, char* name, size_t size) {
  DIR* folder = opendir(trash);
  if (folder == nullptr) {
    return false;
  }
  name[0] = '\0';
  for (dirent* entry = readdir(folder); entry != nullptr; entry = readdir(folder)) {
    if (entry->d_name[0] != '.' && std::strlen(entry->d_name) < size &&
        (name[0] == '\0' || std::strcmp(entry->d_name, name) < 0)) {
      std::memcpy(name, entry->d_name, std::strlen(entry->d_name) + 1);
    }
  }
  closedir(folder);
  return name[0] != '\0';
}

void PurgeTask(void* /*context*/) {
  bool on_card = false;
  const char* base = MediaBasePath(g_purge_storage, false, &on_card);
  if (base != nullptr) {
    char directory[kMaxPath];
    char trash[kMaxPath + 16];
    std::snprintf(directory, sizeof(directory), "%s/%s", base, kPhotoDirectoryName);
    std::snprintf(trash, sizeof(trash), "%s/%s", directory, kPhotoTrashDirectory);
    const uint64_t low = g_low_bytes_for_test != 0 ? g_low_bytes_for_test
        : on_card                                ? kCardLowBytes
                                                 : kInternalLowBytes;
    char name[128];
    int removed = 0;
    uint64_t free_bytes = 0;
    while (FreeBytes(g_purge_storage, base, on_card, &free_bytes) && free_bytes < low &&
           OldestTrashedPhoto(trash, name, sizeof(name)) &&
           DeleteTrashedPhoto(directory, name)) {
      ++removed;
    }
    if (removed > 0) {
      LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
          "Storage low: removed %d photos from Recently Deleted\n", removed);
    }
  }
  g_purge_running.store(false);
  vTaskDelete(nullptr);
}

}  // namespace

const char* OriginalPhotoName(const char* trash_name) {
  if (trash_name == nullptr) {
    return "";
  }
  const char* cursor = trash_name;
  while (*cursor >= '0' && *cursor <= '9') {
    ++cursor;
  }
  return cursor != trash_name && *cursor == '-' ? cursor + 1 : trash_name;
}

bool MovePhotoToTrash(const char* photo_directory, const char* name) {
  char trash[kMaxPath + 16];
  std::snprintf(trash, sizeof(trash), "%s/%s", photo_directory, kPhotoTrashDirectory);
  mkdir(trash, 0775);
  char trash_name[128];
  std::snprintf(trash_name, sizeof(trash_name), "%010lld-%s",
      static_cast<long long>(std::time(nullptr)), name);
  char from[kMaxPath + 80];
  char to[kMaxPath + 160];
  std::snprintf(from, sizeof(from), "%s/%s", photo_directory, name);
  std::snprintf(to, sizeof(to), "%s/%s", trash, trash_name);
  if (rename(from, to) != 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__, "Could not move to trash: %s\n", from);
    return false;
  }
  MoveThumbnail(photo_directory, name, trash, trash_name);
  return true;
}

bool RestorePhotoFromTrash(const char* photo_directory, const char* trash_name) {
  char trash[kMaxPath + 16];
  std::snprintf(trash, sizeof(trash), "%s/%s", photo_directory, kPhotoTrashDirectory);
  const char* original = OriginalPhotoName(trash_name);
  char from[kMaxPath + 160];
  char to[kMaxPath + 160];
  std::snprintf(from, sizeof(from), "%s/%s", trash, trash_name);
  std::snprintf(to, sizeof(to), "%s/%s", photo_directory, original);
  struct stat information {};
  if (stat(to, &information) == 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__, "Already there: %s\n", to);
    return false;
  }
  if (rename(from, to) != 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__, "Could not restore: %s\n", from);
    return false;
  }
  MoveThumbnail(trash, trash_name, photo_directory, original);
  return true;
}

bool DeleteTrashedPhoto(const char* photo_directory, const char* trash_name) {
  char path[kMaxPath + 160];
  std::snprintf(path, sizeof(path), "%s/%s/%s", photo_directory, kPhotoTrashDirectory, trash_name);
  if (unlink(path) != 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__, "Could not delete: %s\n", path);
    return false;
  }
  std::snprintf(path, sizeof(path), "%s/%s/%s/%s.thm", photo_directory, kPhotoTrashDirectory,
      kThumbnailDirectory, trash_name);
  unlink(path);
  return true;
}

int CountTrashedPhotos(const char* photo_directory) {
  char trash[kMaxPath + 16];
  std::snprintf(trash, sizeof(trash), "%s/%s", photo_directory, kPhotoTrashDirectory);
  DIR* folder = opendir(trash);
  if (folder == nullptr) {
    return 0;
  }
  int count = 0;
  for (dirent* entry = readdir(folder); entry != nullptr; entry = readdir(folder)) {
    if (entry->d_name[0] != '.') {
      ++count;
    }
  }
  closedir(folder);
  return count;
}

void SetPhotoTrashLowBytesForTest(uint64_t bytes) { g_low_bytes_for_test = bytes; }

void RequestPhotoTrashPurge(hal::StorageProvider* storage) {
  bool expected = false;
  if (!g_purge_running.compare_exchange_strong(expected, true)) {
    return;
  }
  g_purge_storage = storage;
  if (xTaskCreate(PurgeTask, "trash_purge", 4096, nullptr, 1, nullptr) != pdPASS) {
    g_purge_running.store(false);
  }
}

}  // namespace lilygo_box::app
