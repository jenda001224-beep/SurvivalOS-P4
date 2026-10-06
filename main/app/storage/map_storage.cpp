/**
 * @Description: Map viewport storage
 * @License: GPL 3.0
 */
#include "app/storage/map_storage.h"

#include <array>
#include <cmath>
#include <cstddef>

#include "app/storage/storage_internal.h"
#include "app/storage/tlv_storage.h"
#include "base/logger.h"
#include "esp_err.h"
#include "nvs.h"

namespace lilygo_box::app {
namespace {

constexpr const char* kMapNvsNamespace = "settings";
constexpr const char* kMapNvsKey = "map_view";
constexpr size_t kMapTlvCapacity = 48;
// Coordinates are persisted as millionths of a degree, which is about 11 cm at
// the equator and keeps the TLV container integer-only.
constexpr double kCoordinateScale = 1000000.0;
constexpr uint8_t kMinZoom = 0;
constexpr uint8_t kMaxZoom = 20;

// Allocated field numbers may be kept but never renumbered or reused.
enum class MapField : uint16_t {
  kLatitudeMicro = 1,
  kLongitudeMicro = 2,
  kZoom = 3,
  kValid = 4,
};

MapViewport NormalizeViewport(const MapViewport& source) {
  MapViewport result;
  result.valid = source.valid;
  result.latitude = std::isfinite(source.latitude)
      ? std::min(std::max(source.latitude, -85.0), 85.0)
      : 0.0;
  result.longitude = std::isfinite(source.longitude)
      ? std::min(std::max(source.longitude, -180.0), 180.0)
      : 0.0;
  result.zoom = std::min(std::max(source.zoom, kMinZoom), kMaxZoom);
  return result;
}

bool AreViewportsEqual(const MapViewport& left, const MapViewport& right) {
  return left.valid == right.valid && left.zoom == right.zoom &&
      left.latitude == right.latitude && left.longitude == right.longitude;
}

bool DecodeViewport(
    const storage::TlvBuffer& buffer, MapViewport* viewport) {
  if (viewport == nullptr) {
    return false;
  }
  MapViewport decoded;
  storage::TlvReader reader(
      storage::TlvDomain::kMap, buffer.data.get(), buffer.size);
  storage::TlvField field;
  while (true) {
    const storage::TlvReadResult result = reader.Next(&field);
    if (result == storage::TlvReadResult::kEnd) {
      *viewport = NormalizeViewport(decoded);
      return true;
    }
    if (result == storage::TlvReadResult::kInvalid) {
      return false;
    }
    switch (static_cast<MapField>(field.tag())) {
      case MapField::kLatitudeMicro: {
        int32_t value = 0;
        if (!field.ReadInt32(&value)) {
          return false;
        }
        decoded.latitude = static_cast<double>(value) / kCoordinateScale;
        break;
      }
      case MapField::kLongitudeMicro: {
        int32_t value = 0;
        if (!field.ReadInt32(&value)) {
          return false;
        }
        decoded.longitude = static_cast<double>(value) / kCoordinateScale;
        break;
      }
      case MapField::kZoom: {
        uint8_t value = 0;
        if (!field.ReadUint8(&value)) {
          return false;
        }
        decoded.zoom = value;
        break;
      }
      case MapField::kValid: {
        uint8_t value = 0;
        if (!field.ReadUint8(&value)) {
          return false;
        }
        decoded.valid = value != 0;
        break;
      }
      default:
        break;
    }
  }
}

bool EncodeViewport(const MapViewport& viewport, uint8_t* output,
    size_t capacity, size_t* encoded_size) {
  const MapViewport normalized = NormalizeViewport(viewport);
  storage::TlvWriter writer(storage::TlvDomain::kMap, output, capacity);
  return writer.WriteInt32(static_cast<uint16_t>(MapField::kLatitudeMicro),
             static_cast<int32_t>(normalized.latitude * kCoordinateScale)) &&
      writer.WriteInt32(static_cast<uint16_t>(MapField::kLongitudeMicro),
          static_cast<int32_t>(normalized.longitude * kCoordinateScale)) &&
      writer.WriteUint8(
          static_cast<uint16_t>(MapField::kZoom), normalized.zoom) &&
      writer.WriteUint8(static_cast<uint16_t>(MapField::kValid),
          normalized.valid ? 1 : 0) &&
      writer.Finalize(encoded_size);
}

NvsStorageCache<MapViewport> g_map_cache(
    StorageDomain::kMap, AreViewportsEqual);

}  // namespace

void InitMapCache() {
  MapViewport loaded;
  nvs_handle_t handle = 0;
  if (OpenApplicationNvs(kMapNvsNamespace, NVS_READONLY, &handle) == ESP_OK) {
    storage::TlvBuffer buffer;
    esp_err_t error = ESP_OK;
    const storage::TlvLoadResult result = storage::LoadTlvBuffer(handle,
        kMapNvsKey, storage::TlvDomain::kMap, kMapTlvCapacity, &buffer,
        &error);
    if (result == storage::TlvLoadResult::kLoaded &&
        !DecodeViewport(buffer, &loaded)) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Map TLV payload is invalid\n");
    } else if (result == storage::TlvLoadResult::kError) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Load map TLV failed: %s\n", esp_err_to_name(error));
    }
    nvs_close(handle);
  }
  if (!g_map_cache.Initialize(NormalizeViewport(loaded))) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Initialize map storage cache failed\n");
  }
}

MapViewport GetMapViewport() {
  MapViewport viewport;
  g_map_cache.Read(&viewport);
  return viewport;
}

bool UpdateMapViewport(const MapViewport& viewport) {
  return g_map_cache.UpdateAndPersist(NormalizeViewport(viewport));
}

StorageStageResult StageMapStorage(nvs_handle_t handle) {
  const MapViewport* viewport = nullptr;
  if (!g_map_cache.BeginFlush(&viewport)) {
    return StorageStageResult::kClean;
  }
  std::array<uint8_t, kMapTlvCapacity> buffer = {};
  size_t encoded_size = 0;
  if (!EncodeViewport(*viewport, buffer.data(), buffer.size(),
          &encoded_size) ||
      nvs_set_blob(handle, kMapNvsKey, buffer.data(), encoded_size) !=
          ESP_OK) {
    return StorageStageResult::kFailed;
  }
  return StorageStageResult::kStaged;
}

void FinishMapStorage(bool committed) { g_map_cache.FinishFlush(committed); }

}  // namespace lilygo_box::app
