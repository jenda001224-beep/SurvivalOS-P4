/*
 * @Description: IMU 传感器状态与姿态数据接口
 * @Author: LILYGO_L
 * @Date: 2026-05-14 00:20:00
 * @LastEditTime: 2026-05-14 00:20:00
 * @License: GPL 3.0
 */
#pragma once

namespace lilygo_box::hal {

struct ImuStatus {
  bool ready = false;
  float pitch_deg = 0.0F;
  float yaw_deg = 0.0F;
  float roll_deg = 0.0F;
  // Raw magnetometer reading in microtesla, before the hard-iron offsets are
  // applied. The compass app needs it to run its own calibration.
  float magnetic_x_ut = 0.0F;
  float magnetic_y_ut = 0.0F;
  float magnetic_z_ut = 0.0F;
  // Accelerometer reading in g, used to tell whether the device is level.
  float acceleration_x_g = 0.0F;
  float acceleration_y_g = 0.0F;
  float acceleration_z_g = 0.0F;
};

class ImuProvider {
 public:
  virtual ~ImuProvider() = default;

  virtual bool SetImuEnabled(bool enabled) = 0;

  /**
   * @brief 读取 IMU 运动状态
   * @param status IMU 状态输出地址
   * @return 读取到有效 IMU 状态返回 true，否则返回 false
   */
  virtual bool ReadImuStatus(ImuStatus* status) = 0;

  /**
   * @brief Sets the hard-iron offsets subtracted from the magnetometer.
   * @param offset_x_ut Offset on the X axis in microtesla.
   * @param offset_y_ut Offset on the Y axis in microtesla.
   * @param offset_z_ut Offset on the Z axis in microtesla.
   * @return true when the board supports magnetometer calibration.
   */
  virtual bool SetMagnetometerCalibration(
      float offset_x_ut, float offset_y_ut, float offset_z_ut) {
    (void)offset_x_ut;
    (void)offset_y_ut;
    (void)offset_z_ut;
    return false;
  }
};

}  // namespace lilygo_box::hal
