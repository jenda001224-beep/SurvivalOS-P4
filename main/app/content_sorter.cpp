/*
 * @Description: Files the downloads copied onto the SD card into the folders
 *               the apps read
 * @License: GPL 3.0
 */
#include "app/content_sorter.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "base/logger.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace lilygo_box::app {
namespace {

constexpr size_t kMaxPathLength = 300;
// A card full of photos can have thousands of entries in a folder; the sorter
// only needs to find a handful of large files, so it gives up on a folder
// after this many entries rather than stall.
constexpr int kMaxEntriesPerFolder = 4000;

// Folders that already hold sorted content or belong to another system.
constexpr const char* kSkippedFolders[] = {"wiki", "maps", "Music", "nature", "DCIM",
    "voice", "System Volume Information", "LOST.DIR", "Android", "lilygobox"};

std::atomic<bool> g_sort_running{false};

enum class Kind { kNone, kWiki, kMaps, kMusic, kNature };

/**
 * @brief Whether a name ends in an extension, optionally followed by a
 *        two-letter split suffix (`.zimaa`, `.pmtilesab`).
 */
bool HasExtension(const char* name, const char* extension, bool allow_split) {
  const size_t length = std::strlen(name);
  const size_t extension_length = std::strlen(extension);
  if (length > extension_length &&
      strcasecmp(name + length - extension_length, extension) == 0) {
    return true;
  }
  if (!allow_split || length <= extension_length + 2) {
    return false;
  }
  const char* suffix = name + length - 2;
  return std::islower(static_cast<unsigned char>(suffix[0])) &&
      std::islower(static_cast<unsigned char>(suffix[1])) &&
      strncasecmp(name + length - extension_length - 2, extension,
          extension_length) == 0;
}

Kind Classify(const char* name, bool top_level) {
  // macOS leaves "._name" shadow files next to everything it copies to a FAT
  // or exFAT card; they carry the same extension but no content.
  if (name[0] == '.') {
    return Kind::kNone;
  }
  if (HasExtension(name, ".zim", true)) {
    return Kind::kWiki;
  }
  if (HasExtension(name, ".svnat", true)) {
    return Kind::kNature;
  }
  if (HasExtension(name, ".pmtiles", true) ||
      HasExtension(name, ".svtrails", false)) {
    return Kind::kMaps;
  }
  // Music in the user's own folders is theirs to arrange; only loose files
  // at the top go to /Music.
  if (top_level && HasExtension(name, ".mp3", false)) {
    return Kind::kMusic;
  }
  return Kind::kNone;
}

const char* FolderFor(Kind kind) {
  switch (kind) {
    case Kind::kWiki:
      return "wiki";
    case Kind::kMaps:
      return "maps";
    case Kind::kMusic:
      return "Music";
    case Kind::kNature:
      return "nature";
    case Kind::kNone:
      break;
  }
  return nullptr;
}

bool IsSkippedFolder(const char* name) {
  if (name[0] == '.') {
    return true;
  }
  for (const char* skipped : kSkippedFolders) {
    if (strcasecmp(name, skipped) == 0) {
      return true;
    }
  }
  return false;
}

bool MoveInto(const char* base_path, const char* source, const char* name,
    Kind kind) {
  const char* folder = FolderFor(kind);
  char directory[kMaxPathLength] = {};
  char target[kMaxPathLength] = {};
  if (folder == nullptr ||
      std::snprintf(directory, sizeof(directory), "%s/%s", base_path,
          folder) >= static_cast<int>(sizeof(directory)) ||
      std::snprintf(target, sizeof(target), "%s/%s", directory, name) >=
          static_cast<int>(sizeof(target))) {
    return false;
  }
  mkdir(directory, 0777);
  struct stat info = {};
  if (stat(target, &info) == 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Not moving %s: %s already exists\n", source, target);
    return false;
  }
  if (rename(source, target) != 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Move %s to %s failed\n", source, target);
    return false;
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__, "Moved %s to /%s\n", source,
      folder);
  return true;
}

size_t SortFolder(const char* base_path, const char* folder, bool top_level) {
  DIR* directory = opendir(folder);
  if (directory == nullptr) {
    return 0;
  }
  // Names are collected first: renaming while readdir walks the same folder
  // can make FatFs skip or repeat entries.
  constexpr int kMaxMoves = 64;
  struct Pending {
    char name[128];
    Kind kind;
  };
  static Pending pending[kMaxMoves];
  int pending_count = 0;
  char subfolders[16][128] = {};
  int subfolder_count = 0;

  int seen = 0;
  for (dirent* entry = readdir(directory);
       entry != nullptr && seen < kMaxEntriesPerFolder;
       entry = readdir(directory), ++seen) {
    if (std::strlen(entry->d_name) >= sizeof(pending[0].name)) {
      continue;
    }
    if (entry->d_type == DT_DIR) {
      if (top_level && subfolder_count < 16 && !IsSkippedFolder(entry->d_name)) {
        std::snprintf(subfolders[subfolder_count++], sizeof(subfolders[0]),
            "%s", entry->d_name);
      }
      continue;
    }
    const Kind kind = Classify(entry->d_name, top_level);
    if (kind != Kind::kNone && pending_count < kMaxMoves) {
      std::snprintf(pending[pending_count].name, sizeof(pending[0].name), "%s",
          entry->d_name);
      pending[pending_count].kind = kind;
      ++pending_count;
    }
  }
  closedir(directory);

  size_t moved = 0;
  for (int i = 0; i < pending_count; ++i) {
    char source[kMaxPathLength] = {};
    if (std::snprintf(source, sizeof(source), "%s/%s", folder,
            pending[i].name) < static_cast<int>(sizeof(source)) &&
        MoveInto(base_path, source, pending[i].name, pending[i].kind)) {
      ++moved;
    }
  }
  for (int i = 0; i < subfolder_count; ++i) {
    char path[kMaxPathLength] = {};
    if (std::snprintf(path, sizeof(path), "%s/%s", folder, subfolders[i]) <
        static_cast<int>(sizeof(path))) {
      moved += SortFolder(base_path, path, false);
    }
  }
  return moved;
}

void SortTask(void* context) {
  const auto* base_path = static_cast<const char*>(context);
  const size_t moved = SortCardContent(base_path);
  if (moved > 0) {
    LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
        "Sorted %u file(s) on the SD card\n", static_cast<unsigned>(moved));
  }
  g_sort_running.store(false);
  vTaskDelete(nullptr);
}

}  // namespace

size_t SortCardContent(const char* base_path) {
  if (base_path == nullptr || base_path[0] == '\0') {
    return 0;
  }
  return SortFolder(base_path, base_path, true);
}

void StartCardContentSort(const char* base_path) {
  bool expected = false;
  if (base_path == nullptr ||
      !g_sort_running.compare_exchange_strong(expected, true)) {
    return;
  }
  if (xTaskCreate(SortTask, "sd_sort", 8192, const_cast<char*>(base_path), 2,
          nullptr) != pdPASS) {
    g_sort_running.store(false);
  }
}

bool IsCardContentSortRunning() { return g_sort_running.load(); }

}  // namespace lilygo_box::app
