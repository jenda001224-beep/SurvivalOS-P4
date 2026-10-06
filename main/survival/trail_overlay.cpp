/**
 * @Description: Marked hiking trails drawn over the map
 * @License: GPL 3.0
 */
#include "survival/trail_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "survival/map_geometry.h"

namespace lilygo_box::survival {
namespace {

constexpr char kMagic[] = "SVTRAILS";
constexpr size_t kHeaderBytes = 32;
constexpr uint32_t kSupportedVersion = 1;
constexpr int kTileSize = static_cast<int>(kMapTilePixels);
constexpr double kCoordinateScale = 1e7;
// One cell holds the trails of about 39 km of country; a few hundred kilobytes
// at most, and far less than a map tile costs.
constexpr size_t kMaxCellBytes = 4 * 1024 * 1024;
constexpr uint32_t kMaxCells = 100000;
// A trail leaves the tile far more often than it stays inside it, so segments
// are clipped against a margin rather than tested exactly.
constexpr int kClipMargin = 8;

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

// Waymark colours. The day set matches the paint on the trees; the night set is
// lifted so it still reads on the dark map.
constexpr uint16_t kDayColours[] = {
    Rgb(0xD8, 0x24, 0x24),  // red
    Rgb(0x1F, 0x63, 0xC8),  // blue
    Rgb(0x1E, 0x8E, 0x3E),  // green
    Rgb(0xE0, 0xA8, 0x10),  // yellow
    Rgb(0x7A, 0x4C, 0xC0),  // other
};

constexpr uint16_t kNightColours[] = {
    Rgb(0xFF, 0x6B, 0x6B),
    Rgb(0x74, 0xA8, 0xFF),
    Rgb(0x6D, 0xD5, 0x8C),
    Rgb(0xFF, 0xD1, 0x54),
    Rgb(0xC0, 0x9A, 0xFF),
};

constexpr uint16_t kDayCasing = Rgb(0xFF, 0xFF, 0xFF);
constexpr uint16_t kNightCasing = Rgb(0x10, 0x12, 0x16);

// A point already projected into tile pixels.
struct PixelPoint {
  int x;
  int y;
};

/**
 * @brief Reads a little-endian 32-bit value.
 * @param data Four bytes.
 * @return The value.
 */
uint32_t ReadUint32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
      (static_cast<uint32_t>(data[1]) << 8) |
      (static_cast<uint32_t>(data[2]) << 16) |
      (static_cast<uint32_t>(data[3]) << 24);
}

/**
 * @brief Reads a little-endian signed 32-bit value.
 * @param data Four bytes.
 * @return The value.
 */
int32_t ReadInt32(const uint8_t* data) {
  return static_cast<int32_t>(ReadUint32(data));
}

/**
 * @brief Writes a horizontal run of pixels.
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
 * @param width Pen width.
 * @param color Colour to draw with.
 */
void DrawLine(uint16_t* pixels, PixelPoint from, PixelPoint to, int width,
    uint16_t color) {
  const int dx = std::abs(to.x - from.x);
  const int dy = -std::abs(to.y - from.y);
  const int step_x = from.x < to.x ? 1 : -1;
  const int step_y = from.y < to.y ? 1 : -1;
  int error = dx + dy;
  const int half = width / 2;

  while (true) {
    if (width <= 1) {
      if (from.x >= 0 && from.y >= 0 && from.x < kTileSize &&
          from.y < kTileSize) {
        pixels[from.y * kTileSize + from.x] = color;
      }
    } else {
      for (int offset = -half; offset <= half; ++offset) {
        DrawSpan(pixels, from.y + offset, from.x - half, from.x + half, color);
      }
    }
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
 * @brief Reports whether a segment could touch the tile at all.
 * @param from Start point.
 * @param to End point.
 * @return true when the segment is worth drawing.
 */
bool SegmentTouchesTile(PixelPoint from, PixelPoint to) {
  const int low = -kClipMargin;
  const int high = kTileSize + kClipMargin;
  if (from.x < low && to.x < low) {
    return false;
  }
  if (from.y < low && to.y < low) {
    return false;
  }
  if (from.x > high && to.x > high) {
    return false;
  }
  if (from.y > high && to.y > high) {
    return false;
  }
  return true;
}

/**
 * @brief Picks the stroke width for a trail at a zoom level.
 * @param zoom Zoom level.
 * @return Stroke width in pixels.
 */
int TrailWidth(uint8_t zoom) {
  if (zoom >= 15) {
    return 4;
  }
  if (zoom >= 13) {
    return 3;
  }
  if (zoom >= 11) {
    return 2;
  }
  return 1;
}

}  // namespace

TrailOverlay::~TrailOverlay() { Close(); }

bool TrailOverlay::Open(const char* path) {
  Close();
  if (!file_.Open(path)) {
    return false;
  }

  uint8_t header[kHeaderBytes] = {};
  if (!file_.ReadAt(0, header, sizeof(header)) ||
      std::memcmp(header, kMagic, 8) != 0) {
    Close();
    return false;
  }
  const uint32_t version = ReadUint32(header + 8);
  if (version != kSupportedVersion) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Unsupported trail overlay version %u\n",
        static_cast<unsigned int>(version));
    Close();
    return false;
  }

  grid_zoom_ = header[12];
  cell_count_ = ReadUint32(header + 16);
  line_count_ = ReadUint32(header + 20);
  data_offset_ = ReadUint32(header + 28);
  if (cell_count_ == 0 || cell_count_ > kMaxCells) {
    Close();
    return false;
  }

  const size_t directory_bytes =
      static_cast<size_t>(cell_count_) * sizeof(CellEntry);
  directory_ = static_cast<CellEntry*>(
      heap_caps_malloc(directory_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (directory_ == nullptr) {
    Close();
    return false;
  }
  // The directory is 12 packed bytes per entry on disk, which is not the same
  // layout as the struct, so it is read field by field.
  for (uint32_t index = 0; index < cell_count_; ++index) {
    uint8_t raw[12] = {};
    if (!file_.ReadAt(kHeaderBytes + static_cast<uint64_t>(index) * 12, raw,
            sizeof(raw))) {
      Close();
      return false;
    }
    directory_[index].key = ReadUint32(raw);
    directory_[index].offset = ReadUint32(raw + 4);
    directory_[index].length = ReadUint32(raw + 8);
  }

  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "Trail overlay: %u cells at grid zoom %u, %u segments\n",
      static_cast<unsigned int>(cell_count_),
      static_cast<unsigned int>(grid_zoom_),
      static_cast<unsigned int>(line_count_));
  return true;
}

void TrailOverlay::Close() {
  heap_caps_free(directory_);
  directory_ = nullptr;
  heap_caps_free(cell_data_);
  cell_data_ = nullptr;
  cell_size_ = 0;
  cell_capacity_ = 0;
  cell_key_ = 0xFFFFFFFFU;
  cell_count_ = 0;
  line_count_ = 0;
  file_.Close();
}

bool TrailOverlay::EnsureCellCached(uint32_t key) {
  if (cell_key_ == key) {
    return true;
  }

  uint32_t low = 0;
  uint32_t high = cell_count_ - 1;
  const CellEntry* found = nullptr;
  while (low <= high) {
    const uint32_t middle = low + (high - low) / 2;
    if (directory_[middle].key == key) {
      found = &directory_[middle];
      break;
    }
    if (directory_[middle].key < key) {
      low = middle + 1;
    } else {
      if (middle == 0) {
        break;
      }
      high = middle - 1;
    }
  }

  if (found == nullptr) {
    // Nothing here, and remembering that is what stops a repeated lookup.
    cell_key_ = key;
    cell_size_ = 0;
    return true;
  }
  if (found->length > kMaxCellBytes) {
    return false;
  }

  if (found->length > cell_capacity_) {
    auto* grown = static_cast<uint8_t*>(heap_caps_realloc(
        cell_data_, found->length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (grown == nullptr) {
      return false;
    }
    cell_data_ = grown;
    cell_capacity_ = found->length;
  }
  if (!file_.ReadAt(data_offset_ + found->offset, cell_data_, found->length)) {
    return false;
  }
  cell_size_ = found->length;
  cell_key_ = key;
  return true;
}

uint32_t TrailOverlay::DrawTile(
    uint8_t zoom, uint32_t x, uint32_t y, bool night, uint16_t* pixels) {
  if (!IsOpen() || pixels == nullptr) {
    return 0;
  }

  // The grid is coarser than the tiles being drawn, so one cell covers many
  // tiles; a tile finer than the grid maps onto exactly one cell.
  uint32_t cell_x = x;
  uint32_t cell_y = y;
  if (zoom >= grid_zoom_) {
    const uint8_t shift = static_cast<uint8_t>(zoom - grid_zoom_);
    cell_x = x >> shift;
    cell_y = y >> shift;
  } else {
    // Zoomed out past the grid: several cells fall in one tile, and drawing
    // them all would be slow for something no one can see anyway.
    return 0;
  }
  if (cell_x > 0xFFFF || cell_y > 0xFFFF ||
      !EnsureCellCached((cell_x << 16) | cell_y) || cell_size_ == 0) {
    return 0;
  }

  // Where this tile sits in world pixels, so a coordinate can be projected
  // straight into the bitmap.
  const double origin_x = static_cast<double>(x) * kTileSize;
  const double origin_y = static_cast<double>(y) * kTileSize;
  const int width = TrailWidth(zoom);
  const uint16_t casing = night ? kNightCasing : kDayCasing;

  uint32_t drawn = 0;
  size_t cursor = 0;
  while (cursor + 4 <= cell_size_) {
    const uint8_t colour_index = cell_data_[cursor];
    const uint16_t point_count = static_cast<uint16_t>(
        static_cast<uint16_t>(cell_data_[cursor + 2]) |
        (static_cast<uint16_t>(cell_data_[cursor + 3]) << 8));
    cursor += 4;
    const size_t payload = static_cast<size_t>(point_count) * 8;
    if (point_count < 2 || cursor + payload > cell_size_) {
      break;
    }

    const uint16_t colour = colour_index < 5
        ? (night ? kNightColours[colour_index] : kDayColours[colour_index])
        : (night ? kNightColours[4] : kDayColours[4]);

    // Two passes so the casing of a later segment never paints over the
    // coloured line of an earlier one.
    for (int pass = 0; pass < 2; ++pass) {
      const uint16_t pen = pass == 0 ? casing : colour;
      const int pen_width = pass == 0 ? width + 2 : width;
      PixelPoint previous = {0, 0};
      bool have_previous = false;
      for (uint16_t index = 0; index < point_count; ++index) {
        const size_t offset = cursor + static_cast<size_t>(index) * 8;
        const double longitude =
            ReadInt32(cell_data_ + offset) / kCoordinateScale;
        const double latitude =
            ReadInt32(cell_data_ + offset + 4) / kCoordinateScale;
        const PixelPoint current = {
            static_cast<int>(std::lround(
                LongitudeToPixelX(longitude, zoom) - origin_x)),
            static_cast<int>(std::lround(
                LatitudeToPixelY(latitude, zoom) - origin_y)),
        };
        if (have_previous && SegmentTouchesTile(previous, current)) {
          DrawLine(pixels, previous, current, pen_width, pen);
          if (pass == 1) {
            ++drawn;
          }
        }
        previous = current;
        have_previous = true;
      }
    }
    cursor += payload;
  }
  return drawn;
}

}  // namespace lilygo_box::survival
