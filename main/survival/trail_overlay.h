/**
 * @Description: Marked hiking trails drawn over the map
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "survival/map_renderer.h"
#include "survival/split_file.h"

namespace lilygo_box::survival {

// Waymark colours, matching the Czech hiking network's four.
enum class TrailColour : uint8_t {
  kRed = 0,
  kBlue = 1,
  kGreen = 2,
  kYellow = 3,
  kOther = 4,
};

enum class TrailKind : uint8_t {
  kHiking = 0,
  kCycling = 1,
  kSki = 2,
};

// Reads a `.svtrails` overlay and draws it on top of a rendered map tile.
//
// The file is a grid of cells, each holding the trail segments that cross it,
// so drawing a tile is one binary search plus a walk over a single cell. The
// last cell stays in memory, which means panning across a region reads the card
// only when it crosses a cell boundary. Format details are in docs/TRAILS.md.
class TrailOverlay {
 public:
  TrailOverlay() = default;
  ~TrailOverlay();

  TrailOverlay(const TrailOverlay&) = delete;
  TrailOverlay& operator=(const TrailOverlay&) = delete;

  /**
   * @brief Opens a trail overlay file.
   * @param path Path to the .svtrails file.
   * @return true when the file is usable.
   */
  bool Open(const char* path);

  /**
   * @brief Closes the overlay and frees its cache.
   */
  void Close();

  /**
   * @brief Reports whether an overlay is open.
   * @return true when open.
   */
  bool IsOpen() const { return file_.IsOpen() && cell_count_ > 0; }

  /**
   * @brief Number of trail segments the file holds.
   * @return Segment count.
   */
  uint32_t line_count() const { return line_count_; }

  /**
   * @brief Draws the trails crossing a tile onto an already rendered bitmap.
   * @param zoom Zoom level of the tile.
   * @param x Tile column.
   * @param y Tile row.
   * @param night true to use colours that read on the dark style.
   * @param pixels Destination RGB565 bitmap, kMapTilePixels squared.
   * @return Number of segments drawn.
   */
  uint32_t DrawTile(
      uint8_t zoom, uint32_t x, uint32_t y, bool night, uint16_t* pixels);

 private:
  // One entry of the file's cell directory.
  struct CellEntry {
    uint32_t key = 0;
    uint32_t offset = 0;
    uint32_t length = 0;
  };

  /**
   * @brief Makes sure a grid cell's data is the one held in the cache.
   * @param key Packed cell key.
   * @return true when the cache holds that cell, or it is legitimately empty.
   */
  bool EnsureCellCached(uint32_t key);

  SplitFile file_;
  CellEntry* directory_ = nullptr;
  uint32_t cell_count_ = 0;
  uint32_t line_count_ = 0;
  uint8_t grid_zoom_ = 10;
  uint64_t data_offset_ = 0;

  uint8_t* cell_data_ = nullptr;
  size_t cell_size_ = 0;
  size_t cell_capacity_ = 0;
  uint32_t cell_key_ = 0xFFFFFFFFU;
};

}  // namespace lilygo_box::survival
