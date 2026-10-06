/**
 * @Description: Draws vector map tiles into RGB565 bitmaps
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "survival/mvt_decoder.h"

namespace lilygo_box::survival {

// Size of a rendered tile in pixels. Vector tiles carry no fixed pixel size, so
// this is the resolution the whole map is composed at.
inline constexpr uint32_t kMapTilePixels = 256;

// Colours the map is drawn with. Kept in one place so the whole map can be
// restyled without touching the drawing code.
struct MapStyle {
  uint16_t background = 0;
  uint16_t earth = 0;
  uint16_t water = 0;
  uint16_t landuse = 0;
  // Built-up land, so parks and forests are not the same tone as housing.
  uint16_t urban = 0;
  uint16_t building = 0;
  uint16_t road_fill = 0;
  // Paths and tracks get their own tone so a forest path never reads as a road
  // you could drive down.
  uint16_t path = 0;
  uint16_t rail = 0;
  // Roads are stroked twice, casing first, so they stay legible whatever they
  // cross. Without the casing a light road on light ground disappears.
  uint16_t road_casing = 0;
  uint16_t boundary = 0;
};

/**
 * @brief The daytime map style.
 * @return Style colours.
 */
const MapStyle& DayMapStyle();

/**
 * @brief The night map style, for reading in the dark.
 * @return Style colours.
 */
const MapStyle& NightMapStyle();

/**
 * @brief Draws a decoded vector tile into an RGB565 bitmap.
 *
 * The bitmap must hold kMapTilePixels squared pixels. Polygons are filled with
 * a scanline pass and lines are stroked with a width that follows the zoom, so
 * the result reads as a map rather than as wireframe.
 *
 * When an archive only reaches a coarser zoom than the map is showing, one of
 * its tiles has to cover several finer ones. `divisions` and the sub-tile
 * coordinates select which slice of the tile fills the output, so the geometry
 * is drawn at the finer scale rather than upscaled as pixels.
 *
 * @param tile Decoded tile.
 * @param style Colours to draw with.
 * @param zoom Zoom level being displayed, used to pick line widths.
 * @param divisions How many slices across the tile is split into; 1 draws it
 *        whole.
 * @param sub_x Column of the slice to draw.
 * @param sub_y Row of the slice to draw.
 * @param pixels Destination RGB565 bitmap.
 * @return true when the tile was drawn.
 */
bool RenderMapTile(const MvtTile& tile, const MapStyle& style, uint8_t zoom,
    uint32_t divisions, uint32_t sub_x, uint32_t sub_y, uint16_t* pixels);

/**
 * @brief Fills a bitmap with the style's background colour.
 * @param style Colours to draw with.
 * @param pixels Destination RGB565 bitmap.
 */
void FillMapTileBackground(const MapStyle& style, uint16_t* pixels);

}  // namespace lilygo_box::survival
