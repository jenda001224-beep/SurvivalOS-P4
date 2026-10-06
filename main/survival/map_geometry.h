/**
 * @Description: Web Mercator projection shared by the map app and its tests
 * @License: GPL 3.0
 */
#pragma once

#include <cmath>
#include <cstdint>

#include "survival/map_renderer.h"

namespace lilygo_box::survival {

// The map is addressed in world pixels: the whole world at a zoom level is
// kMapTilePixels * 2^zoom pixels across. Working in pixels rather than degrees
// keeps panning exact and avoids reprojecting on every drag event.

// Web Mercator cannot represent the poles, so latitude is clamped to the
// square world the projection produces.
inline constexpr double kMercatorLatitudeLimit = 85.05112878;

/**
 * @brief Width of the world in pixels at a zoom level.
 * @param zoom Zoom level.
 * @return World span in pixels.
 */
inline double MapWorldSpan(uint8_t zoom) {
  return static_cast<double>(kMapTilePixels) *
      static_cast<double>(1U << zoom);
}

/**
 * @brief Converts a longitude to a world pixel column.
 * @param longitude Longitude in degrees.
 * @param zoom Zoom level.
 * @return World pixel X.
 */
inline double LongitudeToPixelX(double longitude, uint8_t zoom) {
  return (longitude + 180.0) / 360.0 * MapWorldSpan(zoom);
}

/**
 * @brief Converts a latitude to a world pixel row.
 * @param latitude Latitude in degrees.
 * @param zoom Zoom level.
 * @return World pixel Y.
 */
inline double LatitudeToPixelY(double latitude, uint8_t zoom) {
  const double clamped = latitude > kMercatorLatitudeLimit
      ? kMercatorLatitudeLimit
      : (latitude < -kMercatorLatitudeLimit ? -kMercatorLatitudeLimit
                                            : latitude);
  const double radians = clamped * M_PI / 180.0;
  return (1.0 - std::asinh(std::tan(radians)) / M_PI) / 2.0 *
      MapWorldSpan(zoom);
}

/**
 * @brief Converts a world pixel column back to a longitude.
 * @param pixel_x World pixel X.
 * @param zoom Zoom level.
 * @return Longitude in degrees.
 */
inline double PixelXToLongitude(double pixel_x, uint8_t zoom) {
  return pixel_x / MapWorldSpan(zoom) * 360.0 - 180.0;
}

/**
 * @brief Converts a world pixel row back to a latitude.
 * @param pixel_y World pixel Y.
 * @param zoom Zoom level.
 * @return Latitude in degrees.
 */
inline double PixelYToLatitude(double pixel_y, uint8_t zoom) {
  const double n = M_PI * (1.0 - 2.0 * pixel_y / MapWorldSpan(zoom));
  return std::atan(std::sinh(n)) * 180.0 / M_PI;
}

}  // namespace lilygo_box::survival
