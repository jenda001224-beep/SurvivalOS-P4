/**
 * @Description: Bluetooth preference storage
 * @License: GPL 3.0
 */
#include "app/storage/bluetooth_storage.h"

#include <array>
#include <cstddef>
#include <cstdint>

#include "app/storage/storage_internal.h"
#include "app/storage/tlv_storage.h"
#include "base/logger.h"
#include "esp_err.h"
#include "nvs.h"

namespace lilygo_box::app {
namespace {

constexpr const char* kBluetoothNvsNamespace = "settings";
constexpr const char* kBluetoothNvsKey = "bt_config";
constexpr size_t kBluetoothTlvCapacity = 32;

// Allocated field numbers may be kept but never renumbered or reused.
enum class BluetoothField : uint16_t {
  kEnabledRequested = 1,
};

bool AreBluetoothPreferencesEqual(
    const BluetoothPreferences& left, const BluetoothPreferences& right) {
  return left.enabled_requested == right.enabled_requested;
}

bool DecodeBluetoothPreferences(const storage::TlvBuffer& buffer,
    BluetoothPreferences* preferences) {
  if (preferences == nullptr) {
    return false;
  }
  BluetoothPreferences decoded;
  storage::TlvReader reader(
      storage::TlvDomain::kBluetooth, buffer.data.get(), buffer.size);
  storage::TlvField field;
  while (true) {
    const storage::TlvReadResult result = reader.Next(&field);
    if (result == storage::TlvReadResult::kEnd) {
      *preferences = decoded;
      return true;
    }
    if (result == storage::TlvReadResult::kInvalid) {
      return false;
    }
    if (static_cast<BluetoothField>(field.tag()) ==
        BluetoothField::kEnabledRequested) {
      uint8_t value = 0;
      if (!field.ReadUint8(&value)) {
        return false;
      }
      decoded.enabled_requested = value != 0;
    }
  }
}

bool EncodeBluetoothPreferences(const BluetoothPreferences& preferences,
    uint8_t* output, size_t capacity, size_t* encoded_size) {
  storage::TlvWriter writer(
      storage::TlvDomain::kBluetooth, output, capacity);
  return writer.WriteUint8(
             static_cast<uint16_t>(BluetoothField::kEnabledRequested),
             preferences.enabled_requested ? 1 : 0) &&
      writer.Finalize(encoded_size);
}

NvsStorageCache<BluetoothPreferences> g_bluetooth_cache(
    StorageDomain::kBluetooth, AreBluetoothPreferencesEqual);

}  // namespace

void InitBluetoothCache() {
  BluetoothPreferences loaded;
  nvs_handle_t handle = 0;
  if (OpenApplicationNvs(
          kBluetoothNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
    storage::TlvBuffer buffer;
    esp_err_t error = ESP_OK;
    const storage::TlvLoadResult result = storage::LoadTlvBuffer(handle,
        kBluetoothNvsKey, storage::TlvDomain::kBluetooth,
        kBluetoothTlvCapacity, &buffer, &error);
    if (result == storage::TlvLoadResult::kLoaded &&
        !DecodeBluetoothPreferences(buffer, &loaded)) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Bluetooth TLV payload is invalid\n");
    } else if (result == storage::TlvLoadResult::kInvalid) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Bluetooth TLV container is invalid\n");
    } else if (result == storage::TlvLoadResult::kError) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Load Bluetooth TLV failed: %s\n", esp_err_to_name(error));
    }
    nvs_close(handle);
  }
  if (!g_bluetooth_cache.Initialize(loaded)) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Initialize Bluetooth storage cache failed\n");
  }
}

BluetoothPreferences GetBluetoothPreferences() {
  BluetoothPreferences preferences;
  g_bluetooth_cache.Read(&preferences);
  return preferences;
}

bool UpdateBluetoothPreferences(const BluetoothPreferences& preferences) {
  return g_bluetooth_cache.UpdateAndPersist(preferences);
}

StorageStageResult StageBluetoothStorage(nvs_handle_t handle) {
  const BluetoothPreferences* preferences = nullptr;
  if (!g_bluetooth_cache.BeginFlush(&preferences)) {
    return StorageStageResult::kClean;
  }
  std::array<uint8_t, kBluetoothTlvCapacity> buffer = {};
  size_t encoded_size = 0;
  if (!EncodeBluetoothPreferences(*preferences, buffer.data(), buffer.size(),
          &encoded_size) ||
      nvs_set_blob(handle, kBluetoothNvsKey, buffer.data(), encoded_size) !=
          ESP_OK) {
    return StorageStageResult::kFailed;
  }
  return StorageStageResult::kStaged;
}

void FinishBluetoothStorage(bool committed) {
  g_bluetooth_cache.FinishFlush(committed);
}

}  // namespace lilygo_box::app
