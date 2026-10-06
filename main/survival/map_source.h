/**
 * @Description: Tile source over several map archives, with a render cache
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "survival/map_renderer.h"
#include "survival/pmtiles_reader.h"
#include "survival/trail_overlay.h"

namespace lilygo_box::survival {

inline constexpr size_t kMapMaxArchives = 4;
inline constexpr size_t kMapArchiveNameMaxLength = 32;

// One rendered tile held in memory.
struct MapCachedTile {
  uint8_t zoom = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  // RGB565 pixels, kMapTilePixels squared.
  uint16_t* pixels = nullptr;
  // Ticks when this tile was last handed out, for eviction.
  uint32_t last_used = 0;
  bool valid = false;
  // True when the tile had to be drawn from a coarser archive, so the caller
  // can tell the user the detail runs out here.
  bool approximate = false;
};

// Serves rendered map tiles from whatever archives are on the card.
//
// Several archives can sit side by side — a detailed country next to a coarse
// world — and each tile is drawn from the most detailed one that covers it. A
// request beyond an archive's zoom is answered from its nearest parent tile,
// drawn at the finer scale so lines stay crisp instead of pixellated.
class MapSource {
 public:
  MapSource();
  ~MapSource();

  MapSource(const MapSource&) = delete;
  MapSource& operator=(const MapSource&) = delete;

  /**
   * @brief Opens every map archive in a directory.
   * @param directory Directory holding .pmtiles files.
   * @return Number of archives opened.
   */
  size_t OpenDirectory(const char* directory);

  /**
   * @brief Closes every archive and frees the cache.
   */
  void Close();

  /**
   * @brief Number of open archives.
   * @return Archive count.
   */
  size_t archive_count() const { return archive_count_; }

  /**
   * @brief Name of an open archive, from its filename.
   * @param index Archive index.
   * @return The name, or an empty string.
   */
  const char* archive_name(size_t index) const;

  /**
   * @brief Highest zoom any open archive covers.
   * @return Maximum zoom, or 0 when nothing is open.
   */
  uint8_t max_zoom() const;

  /**
   * @brief Sets the style tiles are drawn with; invalidates the cache.
   * @param style Colours to draw with.
   */
  void SetStyle(const MapStyle& style);

  /**
   * @brief Returns a rendered tile, drawing it if it is not cached yet.
   * @param zoom Zoom level.
   * @param x Tile column.
   * @param y Tile row.
   * @param approximate Set when the tile came from a coarser archive.
   * @return The pixels, or nullptr when no archive covers the tile.
   */
  const uint16_t* GetTile(
      uint8_t zoom, uint32_t x, uint32_t y, bool* approximate);

  /**
   * @brief Reports whether a tile is already rendered and cached.
   * @param zoom Zoom level.
   * @param x Tile column.
   * @param y Tile row.
   * @return true when GetTile() would return immediately.
   */
  bool IsCached(uint8_t zoom, uint32_t x, uint32_t y) const;

  /**
   * @brief Drops every cached tile, keeping the archives open.
   */
  void ClearCache();

  /**
   * @brief Reports whether a marked-trail overlay was found.
   * @return true when trails are being drawn.
   */
  bool has_trails() const { return trails_.IsOpen(); }

  /**
   * @brief Turns the marked-trail overlay on or off.
   * @param enabled true to draw trails over the map.
   */
  void SetTrailsEnabled(bool enabled);

  /**
   * @brief Reports whether trails are currently drawn.
   * @return true when the overlay is enabled.
   */
  bool trails_enabled() const { return trails_enabled_; }

 private:
  /**
   * @brief Finds a cache slot for a tile, evicting the oldest if needed.
   * @param zoom Zoom level.
   * @param x Tile column.
   * @param y Tile row.
   * @return The slot, or nullptr when no memory is available.
   */
  MapCachedTile* AcquireSlot(uint8_t zoom, uint32_t x, uint32_t y);

  /**
   * @brief Draws a tile from the best archive that covers it.
   * @param zoom Zoom level.
   * @param x Tile column.
   * @param y Tile row.
   * @param pixels Destination bitmap.
   * @param approximate Set when a coarser archive had to be used.
   * @return true when something was drawn.
   */
  bool RenderInto(uint8_t zoom, uint32_t x, uint32_t y, uint16_t* pixels,
      bool* approximate);

  PmTilesArchive archives_[kMapMaxArchives];
  char archive_names_[kMapMaxArchives][kMapArchiveNameMaxLength] = {};
  // Archive indices ordered most detailed first. The archives themselves are
  // not movable, so the preference order lives here instead.
  size_t order_[kMapMaxArchives] = {};
  size_t archive_count_ = 0;

  MapCachedTile* cache_ = nullptr;
  size_t cache_size_ = 0;
  uint32_t clock_ = 0;
  MapStyle style_;
  TrailOverlay trails_;
  bool trails_enabled_ = true;
  bool night_style_ = false;
};

}  // namespace lilygo_box::survival
