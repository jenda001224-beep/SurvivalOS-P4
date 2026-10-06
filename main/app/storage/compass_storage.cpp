/**
 * @Description: Compass calibration storage
 * @License: GPL 3.0
 */
#include "app/storage/compass_storage.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "app/storage/storage_internal.h"
#include "app/storage/tlv_storage.h"
#include "base/logger.h"
#include "esp_err.h"
#include "nvs.h"

namespace lilygo_box::app {
namespace {

constexpr const char* kCompassNvsNamespace = "settings";
constexpr const char* kCompassNvsKey = "compass_cal";
constexpr size_t kCompassTlvCapacity = 64;
// Offsets are persisted as thousandths of a microtesla so the TLV container can
// stay integer-only.
constexpr float kOffsetScale = 1000.0F;
// Anything beyond this is not a plausible hard-iron offset and is rejected.
constexpr float kOffsetLimitUt = 2000.0F;

// Allocated field numbers may be kept but never renumbered or reused.
enum class CompassField : uint16_t {
  kCalibrated = 1,
  kOffsetXMilliUt = 2,
  kOffsetYMilliUt = 3,
  kOffsetZMilliUt = 4,
};

float NormalizeOffset(float value) {
  if (!std::isfinite(value)) {
    return 0.0F;
  }
  if (value > kOffsetLimitUt) {
    return kOffsetLimitUt;
  }
  if (value < -kOffsetLimitUt) {
    return -kOffsetLimitUt;
  }
  return value;
}

CompassCalibration NormalizeCalibration(const CompassCalibration& source) {
  CompassCalibration result;
  result.calibrated = source.calibrated;
  result.offset_x_ut = NormalizeOffset(source.offset_x_ut);
  result.offset_y_ut = NormalizeOffset(source.offset_y_ut);
  result.offset_z_ut = NormalizeOffset(source.offset_z_ut);
  return result;
}

bool AreCalibrationsEqual(
    const CompassCalibration& left, const CompassCalibration& right) {
  return left.calibrated == right.calibrated &&
      left.offset_x_ut == right.offset_x_ut &&
      left.offset_y_ut == right.offset_y_ut &&
      left.offset_z_ut == right.offset_z_ut;
}

bool DecodeCalibration(
    const storage::TlvBuffer& buffer, CompassCalibration* calibration) {
  if (calibration == nullptr) {
    return false;
  }
  CompassCalibration decoded;
  storage::TlvReader reader(
      storage::TlvDomain::kCompass, buffer.data.get(), buffer.size);
  storage::TlvField field;
  while (true) {
    const storage::TlvReadResult result = reader.Next(&field);
    if (result == storage::TlvReadResult::kEnd) {
      *calibration = NormalizeCalibration(decoded);
      return true;
    }
    if (result == storage::TlvReadResult::kInvalid) {
      return false;
    }
    switch (static_cast<CompassField>(field.tag())) {
      case CompassField::kCalibrated: {
        uint8_t value = 0;
        if (!field.ReadUint8(&value)) {
          return false;
        }
        decoded.calibrated = value != 0;
        break;
      }
      case CompassField::kOffsetXMilliUt:
      case CompassField::kOffsetYMilliUt:
      case CompassField::kOffsetZMilliUt: {
        int32_t value = 0;
        if (!field.ReadInt32(&value)) {
          return false;
        }
        const float offset = static_cast<float>(value) / kOffsetScale;
        if (static_cast<CompassField>(field.tag()) ==
            CompassField::kOffsetXMilliUt) {
          decoded.offset_x_ut = offset;
        } else if (static_cast<CompassField>(field.tag()) ==
            CompassField::kOffsetYMilliUt) {
          decoded.offset_y_ut = offset;
        } else {
          decoded.offset_z_ut = offset;
        }
        break;
      }
      default:
        break;
    }
  }
}

bool EncodeCalibration(const CompassCalibration& calibration, uint8_t* output,
    size_t capacity, size_t* encoded_size) {
  const CompassCalibration normalized = NormalizeCalibration(calibration);
  storage::TlvWriter writer(storage::TlvDomain::kCompass, output, capacity);
  return writer.WriteUint8(static_cast<uint16_t>(CompassField::kCalibrated),
             normalized.calibrated ? 1 : 0) &&
      writer.WriteInt32(
          static_cast<uint16_t>(CompassField::kOffsetXMilliUt),
          static_cast<int32_t>(normalized.offset_x_ut * kOffsetScale)) &&
      writer.WriteInt32(
          static_cast<uint16_t>(CompassField::kOffsetYMilliUt),
          static_cast<int32_t>(normalized.offset_y_ut * kOffsetScale)) &&
      writer.WriteInt32(
          static_cast<uint16_t>(CompassField::kOffsetZMilliUt),
          static_cast<int32_t>(normalized.offset_z_ut * kOffsetScale)) &&
      writer.Finalize(encoded_size);
}

NvsStorageCache<CompassCalibration> g_compass_cache(
    StorageDomain::kCompass, AreCalibrationsEqual);

}  // namespace

void InitCompassCache() {
  CompassCalibration loaded;
  nvs_handle_t handle = 0;
  if (OpenApplicationNvs(kCompassNvsNamespace, NVS_READONLY, &handle) ==
      ESP_OK) {
    storage::TlvBuffer buffer;
    esp_err_t error = ESP_OK;
    const storage::TlvLoadResult result = storage::LoadTlvBuffer(handle,
        kCompassNvsKey, storage::TlvDomain::kCompass, kCompassTlvCapacity,
        &buffer, &error);
    if (result == storage::TlvLoadResult::kLoaded &&
        !DecodeCalibration(buffer, &loaded)) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Compass TLV payload is invalid\n");
    } else if (result == storage::TlvLoadResult::kInvalid) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Compass TLV container is invalid\n");
    } else if (result == storage::TlvLoadResult::kError) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Load compass TLV failed: %s\n", esp_err_to_name(error));
    }
    nvs_close(handle);
  }
  if (!g_compass_cache.Initialize(NormalizeCalibration(loaded))) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Initialize compass storage cache failed\n");
  }
}

CompassCalibration GetCompassCalibration() {
  CompassCalibration calibration;
  g_compass_cache.Read(&calibration);
  return calibration;
}

bool UpdateCompassCalibration(const CompassCalibration& calibration) {
  return g_compass_cache.UpdateAndPersist(NormalizeCalibration(calibration));
}

StorageStageResult StageCompassStorage(nvs_handle_t handle) {
  const CompassCalibration* calibration = nullptr;
  if (!g_compass_cache.BeginFlush(&calibration)) {
    return StorageStageResult::kClean;
  }
  std::array<uint8_t, kCompassTlvCapacity> buffer = {};
  size_t encoded_size = 0;
  if (!EncodeCalibration(*calibration, buffer.data(), buffer.size(),
          &encoded_size) ||
      nvs_set_blob(handle, kCompassNvsKey, buffer.data(), encoded_size) !=
          ESP_OK) {
    return StorageStageResult::kFailed;
  }
  return StorageStageResult::kStaged;
}

void FinishCompassStorage(bool committed) {
  g_compass_cache.FinishFlush(committed);
}

}  // namespace lilygo_box::app
