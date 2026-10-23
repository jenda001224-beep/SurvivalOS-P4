/*
 * @Description: SurvivalOS edition (FULL with an SD card, MINI without) and
 *               the first-run setup state
 * @License: GPL 3.0
 */
#include "app/edition.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#include "app/storage/littlefs_storage.h"
#include "base/logger.h"
#include "hal/providers/storage_provider.h"
#include "nvs.h"

namespace lilygo_box::app {
namespace {

constexpr char kNvsNamespace[] = "svos";
constexpr char kEditionKey[] = "edition";
constexpr char kSetupKey[] = "setup";
// Bumped when the assistant gains a step every phone should see once.
constexpr uint8_t kSetupVersion = 1;
// What internal memory keeps free for settings, saved games and the like.
constexpr size_t kInternalMediaReserveBytes = 640 * 1024;

std::atomic<uint8_t> g_edition{static_cast<uint8_t>(Edition::kFull)};
std::atomic<bool> g_setup_completed{false};

// Apps that only make sense with data on an SD card.
constexpr const char* kCardOnlyApps[] = {"wikipedia", "maps", "music",
    "files"};

uint8_t ReadU8(const char* key, uint8_t fallback) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return fallback;
  }
  uint8_t value = fallback;
  if (nvs_get_u8(handle, key, &value) != ESP_OK) {
    value = fallback;
  }
  nvs_close(handle);
  return value;
}

bool WriteU8(const char* key, uint8_t value) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Save %s failed\n", key);
    return false;
  }
  const bool saved = nvs_set_u8(handle, key, value) == ESP_OK &&
      nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  return saved;
}

}  // namespace

void InitEditionStorage() {
  const uint8_t edition =
      ReadU8(kEditionKey, static_cast<uint8_t>(Edition::kFull));
  g_edition.store(edition == static_cast<uint8_t>(Edition::kMini)
          ? edition
          : static_cast<uint8_t>(Edition::kFull));
  g_setup_completed.store(ReadU8(kSetupKey, 0) >= kSetupVersion);
}

Edition GetEdition() { return static_cast<Edition>(g_edition.load()); }

void SetEdition(Edition edition) {
  g_edition.store(static_cast<uint8_t>(edition));
  WriteU8(kEditionKey, static_cast<uint8_t>(edition));
}

bool IsAppInEdition(const char* app_id) {
  if (app_id == nullptr || GetEdition() == Edition::kFull) {
    return true;
  }
  for (const char* card_only : kCardOnlyApps) {
    if (std::strcmp(app_id, card_only) == 0) {
      return false;
    }
  }
  return true;
}

bool IsSetupCompleted() { return g_setup_completed.load(); }

bool MarkSetupCompleted() {
  g_setup_completed.store(true);
  return WriteU8(kSetupKey, kSetupVersion);
}

void ResetSetup() {
  g_setup_completed.store(false);
  WriteU8(kSetupKey, 0);
}

const char* MediaBasePath(
    hal::StorageProvider* storage, bool mount_card, bool* on_card) {
  if (on_card != nullptr) {
    *on_card = false;
  }
  if (storage != nullptr &&
      (mount_card ? storage->EnsureSdCardMounted()
                  : storage->IsSdCardMounted())) {
    const char* base = storage->SdCardBasePath();
    if (base != nullptr && base[0] != '\0') {
      if (on_card != nullptr) {
        *on_card = true;
      }
      return base;
    }
  }
  if (!IsLittleFsStorageMounted()) {
    return nullptr;
  }
  const char* base = LittleFsStorageBasePath();
  return base != nullptr && base[0] != '\0' ? base : nullptr;
}

bool InternalMediaHasRoom(size_t bytes_needed) {
  size_t total = 0;
  size_t used = 0;
  if (!GetLittleFsStorageInfo(&total, &used) || used > total) {
    return false;
  }
  return total - used >= bytes_needed + kInternalMediaReserveBytes;
}

}  // namespace lilygo_box::app
