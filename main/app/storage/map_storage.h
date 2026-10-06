/**
 * @Description: Map viewport storage
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>

namespace lilygo_box::app {

// Where the map app was last looking, so it opens there again.
struct MapViewport {
  bool valid = false;
  double latitude = 0.0;
  double longitude = 0.0;
  uint8_t zoom = 13;
};

/**
 * @brief Loads the map viewport cache from NVS.
 */
void InitMapCache();

/**
 * @brief Reads the stored map viewport.
 * @return The cached viewport.
 */
MapViewport GetMapViewport();

/**
 * @brief Stores a new map viewport and persists it.
 * @param viewport New viewport snapshot.
 * @return true when the value was unchanged or successfully persisted.
 */
bool UpdateMapViewport(const MapViewport& viewport);

}  // namespace lilygo_box::app
