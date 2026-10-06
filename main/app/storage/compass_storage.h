/**
 * @Description: Compass calibration storage
 * @License: GPL 3.0
 */
#pragma once

namespace lilygo_box::app {

// Hard-iron calibration for the ICM20948 magnetometer. Without it the board's
// own magnetic field pushes the heading off by tens of degrees.
struct CompassCalibration {
  // Whether a calibration has ever been completed on this device.
  bool calibrated = false;
  // Hard-iron offsets in microtesla, subtracted from every raw reading.
  float offset_x_ut = 0.0F;
  float offset_y_ut = 0.0F;
  float offset_z_ut = 0.0F;
};

/**
 * @brief Loads the compass calibration cache from NVS.
 */
void InitCompassCache();

/**
 * @brief Reads the stored compass calibration.
 * @return The cached calibration.
 */
CompassCalibration GetCompassCalibration();

/**
 * @brief Stores a new compass calibration and persists it.
 * @param calibration New calibration snapshot.
 * @return true when the value was unchanged or successfully persisted.
 */
bool UpdateCompassCalibration(const CompassCalibration& calibration);

}  // namespace lilygo_box::app
