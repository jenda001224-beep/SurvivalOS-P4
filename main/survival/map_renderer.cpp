/**
 * @Description: Draws vector map tiles into RGB565 bitmaps
 * @License: GPL 3.0
 */
#include "survival/map_renderer.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "esp_heap_caps.h"

namespace lilygo_box::survival {
namespace {

constexpr int kTileSize = static_cast<int>(kMapTilePixels);
// A polygon with more edges than this in one scanline is not something worth
// drawing pixel-perfect on a phone-sized map.
constexpr uint32_t kMaxCrossings = 256;

/**
 * @brief Packs a colour into RGB565.
 * @param red Red channel.
 * @param green Green channel.
 * @param blue Blue channel.
 * @return The packed colour.
 */
constexpr uint16_t Rgb(uint8_t red, uint8_t green, uint8_t blue) {
  return static_cast<uint16_t>(((red & 0xF8) << 8) | ((green & 0xFC) << 3) |
      (blue >> 3));
}

// Which layers get drawn, in the order they are painted. Anything the basemap
// contains but this list leaves out simply does not appear.
enum class LayerRole : uint8_t {
  kSkip,
  kEarth,
  kLanduse,
  kWater,
  kBuilding,
  kRoad,
  kBoundary,
};

/**
 * @brief Maps a basemap layer name to how it should be drawn.
 * @param name Layer name from the tile.
 * @return The drawing role.
 */
LayerRole RoleForLayer(const char* name) {
  if (std::strcmp(name, "earth") == 0) {
    return LayerRole::kEarth;
  }
  if (std::strcmp(name, "landuse") == 0 || std::strcmp(name, "natural") == 0) {
    return LayerRole::kLanduse;
  }
  if (std::strcmp(name, "water") == 0) {
    return LayerRole::kWater;
  }
  if (std::strcmp(name, "buildings") == 0) {
    return LayerRole::kBuilding;
  }
  if (std::strcmp(name, "roads") == 0 || std::strcmp(name, "transit") == 0) {
    return LayerRole::kRoad;
  }
  if (std::strcmp(name, "boundaries") == 0) {
    return LayerRole::kBoundary;
  }
  return LayerRole::kSkip;
}

// How prominently a road is drawn, from its `kind` tag.
enum class RoadClass : uint8_t {
  kNone,
  kHighway,
  kMajor,
  kMedium,
  kMinor,
  kTrack,
  kPath,
  kRail,
  kFerry,
};

/**
 * @brief Classifies a road from its kind tag.
 * @param kind Kind string from the tile.
 * @return The road class.
 */
RoadClass ClassifyRoad(const char* kind) {
  if (std::strcmp(kind, "highway") == 0) {
    return RoadClass::kHighway;
  }
  if (std::strcmp(kind, "major_road") == 0) {
    return RoadClass::kMajor;
  }
  if (std::strcmp(kind, "medium_road") == 0) {
    return RoadClass::kMedium;
  }
  if (std::strcmp(kind, "minor_road") == 0) {
    return RoadClass::kMinor;
  }
  if (std::strcmp(kind, "path") == 0) {
    return RoadClass::kPath;
  }
  if (std::strcmp(kind, "rail") == 0) {
    return RoadClass::kRail;
  }
  if (std::strcmp(kind, "ferry") == 0) {
    return RoadClass::kFerry;
  }
  // Everything the basemap could not place — tracks, service roads, driveways.
  // On a survival map these matter, so they are drawn rather than dropped.
  return RoadClass::kTrack;
}

/**
 * @brief Picks the stroke width for a road.
 *
 * Widths grow with zoom and with importance, which is what stops a city at
 * zoom 12 from turning into a solid mat of lines.
 *
 * @param road Road class.
 * @param zoom Zoom level.
 * @return Stroke width in pixels, or 0 when the road should not be drawn.
 */
int RoadWidth(RoadClass road, uint8_t zoom) {
  switch (road) {
    case RoadClass::kHighway:
      return zoom >= 13 ? 5 : (zoom >= 9 ? 3 : 2);
    case RoadClass::kMajor:
      return zoom >= 14 ? 4 : (zoom >= 13 ? 2 : 1);
    case RoadClass::kMedium:
      return zoom >= 14 ? 3 : (zoom >= 12 ? 2 : 1);
    case RoadClass::kMinor:
      return zoom >= 15 ? 3 : (zoom >= 13 ? 2 : 1);
    case RoadClass::kTrack:
      return zoom >= 14 ? 2 : (zoom >= 12 ? 1 : 0);
    case RoadClass::kPath:
      return zoom >= 13 ? 2 : (zoom >= 12 ? 1 : 0);
    case RoadClass::kRail:
      return zoom >= 12 ? 1 : 0;
    case RoadClass::kFerry:
      return zoom >= 9 ? 1 : 0;
    default:
      return 0;
  }
}

/**
 * @brief Reports whether a road class is drawn as a broken line.
 *
 * Paths, tracks and ferries are dashed the way a walking map draws them, so a
 * forest path is never mistaken for a road you could drive.
 *
 * @param road Road class.
 * @return true when the line should be dashed.
 */
bool IsDashedRoad(RoadClass road) {
  return road == RoadClass::kPath || road == RoadClass::kTrack ||
      road == RoadClass::kFerry;
}

/**
 * @brief Picks the colour a road class is filled with.
 * @param road Road class.
 * @param style Colours to draw with.
 * @return The fill colour.
 */
uint16_t RoadColor(RoadClass road, const MapStyle& style) {
  if (road == RoadClass::kPath || road == RoadClass::kTrack) {
    return style.path;
  }
  if (road == RoadClass::kRail || road == RoadClass::kFerry) {
    return style.rail;
  }
  return style.road_fill;
}

// A point already converted to tile pixels.
struct PixelPoint {
  int x;
  int y;
};

// How the tile's coordinate space maps onto the output bitmap.
struct TileTransform {
  uint32_t extent = 4096;
  uint32_t divisions = 1;
  uint32_t sub_x = 0;
  uint32_t sub_y = 0;
};

/**
 * @brief Converts a tile-local X coordinate to a pixel in the output bitmap.
 * @param value Coordinate in the layer's extent space.
 * @param transform Active tile transform.
 * @return The pixel coordinate.
 */
int ToPixelX(int value, const TileTransform& transform) {
  if (transform.extent == 0) {
    return 0;
  }
  const int64_t scaled = static_cast<int64_t>(value) * kTileSize *
      transform.divisions / static_cast<int>(transform.extent);
  return static_cast<int>(scaled -
      static_cast<int64_t>(transform.sub_x) * kTileSize);
}

/**
 * @brief Converts a tile-local Y coordinate to a pixel in the output bitmap.
 * @param value Coordinate in the layer's extent space.
 * @param transform Active tile transform.
 * @return The pixel coordinate.
 */
int ToPixelY(int value, const TileTransform& transform) {
  if (transform.extent == 0) {
    return 0;
  }
  const int64_t scaled = static_cast<int64_t>(value) * kTileSize *
      transform.divisions / static_cast<int>(transform.extent);
  return static_cast<int>(scaled -
      static_cast<int64_t>(transform.sub_y) * kTileSize);
}

/**
 * @brief Writes one pixel if it lands inside the bitmap.
 * @param pixels Destination bitmap.
 * @param x Column.
 * @param y Row.
 * @param color Colour to write.
 */
inline void PutPixel(uint16_t* pixels, int x, int y, uint16_t color) {
  if (x < 0 || y < 0 || x >= kTileSize || y >= kTileSize) {
    return;
  }
  pixels[y * kTileSize + x] = color;
}

/**
 * @brief Draws a horizontal run of pixels.
 * @param pixels Destination bitmap.
 * @param y Row.
 * @param from First column.
 * @param to Last column.
 * @param color Colour to write.
 */
void DrawSpan(uint16_t* pixels, int y, int from, int to, uint16_t color) {
  if (y < 0 || y >= kTileSize) {
    return;
  }
  from = std::max(from, 0);
  to = std::min(to, kTileSize - 1);
  uint16_t* row = pixels + y * kTileSize;
  for (int x = from; x <= to; ++x) {
    row[x] = color;
  }
}

/**
 * @brief Strokes a line with a square pen.
 * @param pixels Destination bitmap.
 * @param from Start point.
 * @param to End point.
 * @param width Pen width in pixels.
 * @param color Colour to draw with.
 */
void DrawLine(uint16_t* pixels, PixelPoint from, PixelPoint to, int width,
    uint16_t color, int dash, int* dash_phase) {
  // Bresenham, with the pen widened by drawing a small square at each step.
  int dx = std::abs(to.x - from.x);
  int dy = -std::abs(to.y - from.y);
  const int step_x = from.x < to.x ? 1 : -1;
  const int step_y = from.y < to.y ? 1 : -1;
  int error = dx + dy;
  const int half = width / 2;

  while (true) {
    // A dash of zero draws solid; otherwise the pen lifts for every other run.
    const bool inked = dash <= 0 || ((*dash_phase / dash) % 2) == 0;
    if (inked) {
      if (width <= 1) {
        PutPixel(pixels, from.x, from.y, color);
      } else {
        for (int offset_y = -half; offset_y <= half; ++offset_y) {
          DrawSpan(pixels, from.y + offset_y, from.x - half, from.x + half,
              color);
        }
      }
    }
    ++(*dash_phase);
    if (from.x == to.x && from.y == to.y) {
      return;
    }
    const int doubled = 2 * error;
    if (doubled >= dy) {
      error += dy;
      from.x += step_x;
    }
    if (doubled <= dx) {
      error += dx;
      from.y += step_y;
    }
  }
}

/**
 * @brief Fills a polygon made of one or more rings.
 *
 * Uses an even-odd scanline fill, which handles holes the way vector tiles
 * encode them without needing winding rules.
 *
 * @param tile Decoded tile the rings belong to.
 * @param feature Feature to fill.
 * @param transform Active tile transform.
 * @param pixels Destination bitmap.
 * @param color Colour to fill with.
 * @param crossings Scratch array of at least kMaxCrossings entries.
 */
void FillPolygon(const MvtTile& tile, const MvtFeature& feature,
    const TileTransform& transform, uint16_t* pixels, uint16_t color,
    int* crossings) {
  int minimum_y = kTileSize;
  int maximum_y = 0;
  for (uint32_t index = 0; index < feature.ring_count; ++index) {
    const MvtRing* ring = tile.ring(feature.first_ring + index);
    if (ring == nullptr) {
      continue;
    }
    const MvtPoint* points = tile.points(*ring);
    for (uint32_t point = 0; point < ring->point_count; ++point) {
      const int y = ToPixelY(points[point].y, transform);
      minimum_y = std::min(minimum_y, y);
      maximum_y = std::max(maximum_y, y);
    }
  }
  minimum_y = std::max(minimum_y, 0);
  maximum_y = std::min(maximum_y, kTileSize - 1);

  for (int y = minimum_y; y <= maximum_y; ++y) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < feature.ring_count; ++index) {
      const MvtRing* ring = tile.ring(feature.first_ring + index);
      if (ring == nullptr || ring->point_count < 3) {
        continue;
      }
      const MvtPoint* points = tile.points(*ring);
      for (uint32_t point = 0; point < ring->point_count; ++point) {
        const uint32_t next =
            point + 1 == ring->point_count ? 0 : point + 1;
        const int y0 = ToPixelY(points[point].y, transform);
        const int y1 = ToPixelY(points[next].y, transform);
        if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
          const int x0 = ToPixelX(points[point].x, transform);
          const int x1 = ToPixelX(points[next].x, transform);
          if (count < kMaxCrossings) {
            crossings[count++] =
                x0 + (y - y0) * (x1 - x0) / (y1 - y0 == 0 ? 1 : y1 - y0);
          }
        }
      }
    }
    if (count < 2) {
      continue;
    }
    std::sort(crossings, crossings + count);
    for (uint32_t index = 0; index + 1 < count; index += 2) {
      DrawSpan(pixels, y, crossings[index], crossings[index + 1], color);
    }
  }
}

/**
 * @brief Strokes every ring of a feature.
 * @param tile Decoded tile the rings belong to.
 * @param feature Feature to stroke.
 * @param transform Active tile transform.
 * @param pixels Destination bitmap.
 * @param width Pen width.
 * @param color Colour to draw with.
 */
void StrokeFeature(const MvtTile& tile, const MvtFeature& feature,
    const TileTransform& transform, uint16_t* pixels, int width,
    uint16_t color, int dash = 0) {
  for (uint32_t index = 0; index < feature.ring_count; ++index) {
    const MvtRing* ring = tile.ring(feature.first_ring + index);
    if (ring == nullptr || ring->point_count < 2) {
      continue;
    }
    const MvtPoint* points = tile.points(*ring);
    PixelPoint previous = {
        ToPixelX(points[0].x, transform), ToPixelY(points[0].y, transform)};
    // The dash phase runs along the whole ring so the pattern does not restart
    // at every vertex.
    int dash_phase = 0;
    for (uint32_t point = 1; point < ring->point_count; ++point) {
      const PixelPoint current = {
          ToPixelX(points[point].x, transform),
          ToPixelY(points[point].y, transform)};
      DrawLine(pixels, previous, current, width, color, dash, &dash_phase);
      previous = current;
    }
  }
}

/**
 * @brief Picks the fill for a landuse feature.
 * @param kind Kind string from the tile.
 * @param style Colours to draw with.
 * @return The fill colour.
 */
uint16_t LanduseColor(const char* kind, const MapStyle& style) {
  if (std::strcmp(kind, "forest") == 0 || std::strcmp(kind, "wood") == 0 ||
      std::strcmp(kind, "park") == 0 || std::strcmp(kind, "grass") == 0 ||
      std::strcmp(kind, "village_green") == 0 ||
      std::strcmp(kind, "garden") == 0 || std::strcmp(kind, "scrub") == 0 ||
      std::strcmp(kind, "meadow") == 0) {
    return style.landuse;
  }
  // Built-up land sits closer to the ground colour so greenery stands out.
  return style.urban;
}

constexpr MapStyle kDayStyle = {
    .background = Rgb(0xE4, 0xDF, 0xD5),
    .earth = Rgb(0xEC, 0xE7, 0xDC),
    .water = Rgb(0x9C, 0xC0, 0xE4),
    .landuse = Rgb(0xC8, 0xDF, 0xB6),
    .urban = Rgb(0xE6, 0xE0, 0xD4),
    .building = Rgb(0xD2, 0xCA, 0xBE),
    .road_fill = Rgb(0xFF, 0xFF, 0xFF),
    .path = Rgb(0x9A, 0x6B, 0x4A),
    .rail = Rgb(0x8A, 0x86, 0x80),
    .road_casing = Rgb(0xB3, 0xAA, 0x9C),
    .boundary = Rgb(0xA8, 0x92, 0xB4),
};

constexpr MapStyle kNightStyle = {
    .background = Rgb(0x14, 0x16, 0x1A),
    .earth = Rgb(0x1C, 0x1F, 0x24),
    .water = Rgb(0x16, 0x2A, 0x40),
    .landuse = Rgb(0x1B, 0x28, 0x1B),
    .urban = Rgb(0x21, 0x24, 0x2A),
    .building = Rgb(0x2A, 0x2D, 0x33),
    .road_fill = Rgb(0x8A, 0x90, 0x9C),
    .path = Rgb(0xC0, 0x8E, 0x62),
    .rail = Rgb(0x60, 0x64, 0x6C),
    .road_casing = Rgb(0x2A, 0x2E, 0x36),
    .boundary = Rgb(0x50, 0x44, 0x58),
};

}  // namespace

const MapStyle& DayMapStyle() { return kDayStyle; }

const MapStyle& NightMapStyle() { return kNightStyle; }

void FillMapTileBackground(const MapStyle& style, uint16_t* pixels) {
  if (pixels == nullptr) {
    return;
  }
  for (uint32_t index = 0; index < kMapTilePixels * kMapTilePixels; ++index) {
    pixels[index] = style.background;
  }
}

bool RenderMapTile(const MvtTile& tile, const MapStyle& style, uint8_t zoom,
    uint32_t divisions, uint32_t sub_x, uint32_t sub_y, uint16_t* pixels) {
  if (pixels == nullptr || divisions == 0) {
    return false;
  }
  FillMapTileBackground(style, pixels);

  auto* crossings = static_cast<int*>(heap_caps_malloc(
      kMaxCrossings * sizeof(int), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (crossings == nullptr) {
    return false;
  }

  // Painted back to front: ground, then what sits on it, then what crosses it.
  static constexpr LayerRole kOrder[] = {
      LayerRole::kEarth, LayerRole::kLanduse, LayerRole::kWater,
      LayerRole::kBuilding, LayerRole::kRoad, LayerRole::kBoundary};

  for (LayerRole role : kOrder) {
    for (uint32_t index = 0; index < tile.layer_count(); ++index) {
      const MvtLayer* layer = tile.layer(index);
      if (layer == nullptr || RoleForLayer(layer->name) != role) {
        continue;
      }

      const TileTransform transform = {
          layer->extent, divisions, sub_x, sub_y};
      uint16_t color = style.earth;
      switch (role) {
        case LayerRole::kEarth:
          color = style.earth;
          break;
        case LayerRole::kLanduse:
          color = style.landuse;
          break;  // Refined per feature below.
        case LayerRole::kWater:
          color = style.water;
          break;
        case LayerRole::kBuilding:
          color = style.building;
          break;
        case LayerRole::kRoad:
          color = style.road_fill;
          break;
        case LayerRole::kBoundary:
          color = style.boundary;
          break;
        default:
          continue;
      }

      for (uint32_t offset = 0; offset < layer->feature_count; ++offset) {
        const MvtFeature* feature =
            tile.feature(layer->first_feature + offset);
        if (feature == nullptr) {
          continue;
        }
        // The basemap tags every feature with the zoom it becomes relevant at.
        if (feature->min_zoom > zoom) {
          continue;
        }
        if (feature->type == MvtGeometryType::kPolygon) {
          const uint16_t fill = role == LayerRole::kLanduse
              ? LanduseColor(tile.kind_name(feature->kind), style)
              : color;
          FillPolygon(
              tile, *feature, transform, pixels, fill, crossings);
          // A thin outline keeps adjacent buildings from merging into a blob.
          if (role == LayerRole::kBuilding) {
            StrokeFeature(
                tile, *feature, transform, pixels, 1, style.background);
          }
        } else if (feature->type == MvtGeometryType::kLine) {
          if (role == LayerRole::kRoad) {
            // Casing first, then the fill on top of it, so the road reads as a
            // road however light the ground under it is.
            const RoadClass road =
                ClassifyRoad(tile.kind_name(feature->kind));
            const int width = RoadWidth(road, zoom);
            // A dashed line has no casing: the gaps would fill with outline and
            // the dashes would disappear.
            if (width > 0 && !IsDashedRoad(road)) {
              // Thin roads only get a one pixel casing, or the outline is
              // thicker than the road it is outlining.
              StrokeFeature(tile, *feature, transform, pixels,
                  width + (width <= 2 ? 1 : 2), style.road_casing);
            }
          } else {
            StrokeFeature(tile, *feature, transform, pixels, 1, color);
          }
        }
      }
    }
  }

  for (uint32_t index = 0; index < tile.layer_count(); ++index) {
    const MvtLayer* layer = tile.layer(index);
    if (layer == nullptr || RoleForLayer(layer->name) != LayerRole::kRoad) {
      continue;
    }
    const TileTransform transform = {layer->extent, divisions, sub_x, sub_y};
    for (uint32_t offset = 0; offset < layer->feature_count; ++offset) {
      const MvtFeature* feature = tile.feature(layer->first_feature + offset);
      if (feature == nullptr || feature->type != MvtGeometryType::kLine ||
          feature->min_zoom > zoom) {
        continue;
      }
      const RoadClass road = ClassifyRoad(tile.kind_name(feature->kind));
      const int width = RoadWidth(road, zoom);
      if (width > 0) {
        // Dash lengths are in pen steps, so they stay even along diagonals.
        const int dash = IsDashedRoad(road) ? (zoom >= 14 ? 4 : 3) : 0;
        StrokeFeature(tile, *feature, transform, pixels, width,
            RoadColor(road, style), dash);
      }
    }
  }

  heap_caps_free(crossings);
  return true;
}

}  // namespace lilygo_box::survival
