/*
 * @Description: T-Display-P4 ICM20948 IMU 实现
 * @Author: LILYGO_L
 * @Date: 2026-08-28 00:00:00
 * @LastEditTime: 2026-09-02 17:53:04
 * @License: GPL 3.0
 */
// SurvivalOS: this is upstream's later V1 implementation (lilygobox-espidf
// 5db8622, "add compass app and improve IMU diagnostics"), reduced to the
// ICM20948 that the v2.2.0 device driver in this tree supports. Compared with
// v1.1.2 it aligns the magnetometer axes with the accelerometer, keeps the
// last valid magnetometer sample between its slower updates, and reports the
// gyroscope and a sample time so the compass can fuse them.
#include <cmath>
#include <cstdint>

#include "esp_timer.h"

#include "hal/device/t_display_p4/device.h"

namespace lilygo_box::hal {
namespace {

constexpr float kDegreesToRadians = 0.0174532925F;
constexpr float kRadiansToDegrees = 57.2957795F;

struct Vector3 {
  float x = 0.0F;
  float y = 0.0F;
  float z = 0.0F;
};

struct ImuSample {
  Vector3 acceleration_g;
  Vector3 magnetic_field_ut;
  Vector3 angular_velocity_dps;
  bool magnetic_field_ready = false;
};

Vector3 g_magnetic_field;
bool g_magnetic_ready = false;

bool ReadIcm20948(cpp_bus_driver::Icm20948& sensor, ImuSample& sample) {
  cpp_bus_driver::Icm20948::RawData raw;
  if (!sensor.ReadRawData(raw)) {
    return false;
  }
  // 对应板级默认 ±2g 量程；AK09916 的灵敏度为 0.15 uT/LSB。
  sample.acceleration_g = {raw.acceleration.x / 16384.0F,
      raw.acceleration.y / 16384.0F, raw.acceleration.z / 16384.0F};
  // 对应板级默认 ±250 deg/s 量程。
  sample.angular_velocity_dps = {raw.angular_velocity.x / 131.0F,
      raw.angular_velocity.y / 131.0F, raw.angular_velocity.z / 131.0F};
  sample.magnetic_field_ready = raw.magnetometer_data_ready &&
                               !raw.magnetometer_overflow;
  // 内置 AK09916 与 ICM20948 六轴的坐标方向不同，按 (X, -Y, -Z) 对齐。
  // 在缓存、姿态计算和状态输出前完成轴向转换。
  sample.magnetic_field_ut = {raw.magnetic_field.x * 0.15F,
      -raw.magnetic_field.y * 0.15F, -raw.magnetic_field.z * 0.15F};
  return true;
}

}  // namespace

bool TDisplayP4Device::SetImuEnabled(bool enabled) {
  g_magnetic_ready = false;
  const bool result = driver_.SetIcm20948Sleep(!enabled);
  imu_enabled_.store(enabled && result);
  return result;
}

bool TDisplayP4Device::ReadImuStatus(ImuStatus* status) {
  if (status == nullptr) {
    return false;
  }

  *status = ImuStatus();
  auto& icm20948 = driver_.chip().icm20948;
  if (!imu_enabled_.load() || !driver_.IsIcm20948Ready() ||
      icm20948 == nullptr) {
    return false;
  }

  ImuSample data;
  if (!ReadIcm20948(*icm20948, data)) {
    return false;
  }

  if (data.magnetic_field_ready) {
    g_magnetic_field = data.magnetic_field_ut;
    g_magnetic_ready = true;
  }
  if (!g_magnetic_ready) {
    return false;
  }
  const auto& acceleration = data.acceleration_g;
  const float acceleration_magnitude_squared = acceleration.x * acceleration.x +
                                               acceleration.y * acceleration.y +
                                               acceleration.z * acceleration.z;
  const auto& magnetic = g_magnetic_field;
  const float magnetic_magnitude_squared = magnetic.x * magnetic.x +
                                           magnetic.y * magnetic.y +
                                           magnetic.z * magnetic.z;
  if (acceleration_magnitude_squared < 0.0001F ||
      magnetic_magnitude_squared < 0.0001F) {
    return false;
  }

  const float pitch =
      std::atan2(-acceleration.x, std::sqrt(acceleration.y * acceleration.y +
                                            acceleration.z * acceleration.z)) *
      kRadiansToDegrees;
  const float roll =
      std::atan2(acceleration.y, acceleration.z) * kRadiansToDegrees;
  const float pitch_radians = pitch * kDegreesToRadians;
  const float roll_radians = roll * kDegreesToRadians;
  const float magnetic_x_horizontal = magnetic.x * std::cos(pitch_radians) +
                                      magnetic.z * std::sin(pitch_radians);
  const float magnetic_y_horizontal =
      magnetic.x * std::sin(roll_radians) * std::sin(pitch_radians) +
      magnetic.y * std::cos(roll_radians) -
      magnetic.z * std::sin(roll_radians) * std::cos(pitch_radians);
  float yaw = std::atan2(magnetic_y_horizontal, magnetic_x_horizontal) *
              kRadiansToDegrees;
  if (yaw < 0.0F) {
    yaw += 360.0F;
  }

  status->sample_time_us = esp_timer_get_time();
  status->acceleration_g[0] = acceleration.x;
  status->acceleration_g[1] = acceleration.y;
  status->acceleration_g[2] = acceleration.z;
  status->angular_velocity_ready = true;
  status->angular_velocity_dps[0] = data.angular_velocity_dps.x;
  status->angular_velocity_dps[1] = data.angular_velocity_dps.y;
  status->angular_velocity_dps[2] = data.angular_velocity_dps.z;
  status->ready = true;
  status->pitch_deg = pitch;
  status->yaw_deg = yaw;
  status->roll_deg = roll;
  status->magnetic_field_ready = data.magnetic_field_ready;
  status->magnetic_field_ut[0] = magnetic.x;
  status->magnetic_field_ut[1] = magnetic.y;
  status->magnetic_field_ut[2] = magnetic.z;
  return true;
}

float TDisplayP4Device::ConvertImuHeading(float yaw_deg) const {
  float heading = std::fmod(-yaw_deg, 360.0F);
  if (heading < 0.0F) {
    heading += 360.0F;
  }
  return heading;
}

}  // namespace lilygo_box::hal
