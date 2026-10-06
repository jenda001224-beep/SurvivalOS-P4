/**
 * @Description: Tile source over several map archives, with a render cache
 * @License: GPL 3.0
 */
#include "survival/map_source.h"

#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "survival/mvt_decoder.h"

namespace lilygo_box::survival {
namespace {

// Enough for a screenful plus what a pan is about to reveal. At 256x256 RGB565
// each tile is 128 kB, so this is about 4 MB of PSRAM.
constexpr size_t kCacheTiles = 32;
constexpr size_t kTileBytes =
    static_cast<size_t>(kMapTilePixels) * kMapTilePixels * sizeof(uint16_t);
// How far up the pyramid to look when an archive stops short of the zoom being
// displayed. Beyond five levels one tile is covering a thousand, and the result
// is not worth drawing.
constexpr uint8_t kMaxParentLevels = 5;
constexpr size_t kMaxPathLength = 224;

}  // namespace

MapSource::MapSource() { style_ = DayMapStyle(); }

MapSource::~MapSource() { Close(); }

const char* MapSource::archive_name(size_t index) const {
  return index < archive_count_ ? archive_names_[index] : "";
}

uint8_t MapSource::max_zoom() const {
  uint8_t highest = 0;
  for (size_t index = 0; index < archive_count_; ++index) {
    highest = std::max(highest, archives_[index].max_zoom());
  }
  return highest;
}

void MapSource::SetStyle(const MapStyle& style) {
  style_ = style;
  // The trail overlay picks its colours from whether the map is dark, and the
  // background is the cheapest thing to test that against.
  night_style_ = style.background < NightMapStyle().earth;
  ClearCache();
}

void MapSource::SetTrailsEnabled(bool enabled) {
  if (trails_enabled_ == enabled) {
    return;
  }
  trails_enabled_ = enabled;
  ClearCache();
}

size_t MapSource::OpenDirectory(const char* directory) {
  Close();
  if (directory == nullptr) {
    return 0;
  }

  DIR* handle = opendir(directory);
  if (handle == nullptr) {
    return 0;
  }
  while (archive_count_ < kMapMaxArchives) {
    const dirent* entry = readdir(handle);
    if (entry == nullptr) {
      break;
    }
    const size_t length = std::strlen(entry->d_name);
    const bool whole = length > 8 &&
        strncasecmp(entry->d_name + length - 8, ".pmtiles", 8) == 0;
    const bool first_part = length > 10 &&
        strncasecmp(entry->d_name + length - 10, ".pmtilesaa", 10) == 0;
    if (!whole && !first_part) {
      continue;
    }

    char base[kMaxPathLength] = {};
    // SplitFile wants the name without the part suffix.
    const size_t base_length =
        std::min(first_part ? length - 2 : length, sizeof(base) - 1);
    std::memcpy(base, entry->d_name, base_length);
    base[base_length] = '\0';

    char path[kMaxPathLength] = {};
    if (std::snprintf(path, sizeof(path), "%s/%s", directory, base) >=
        static_cast<int>(sizeof(path))) {
      continue;
    }
    if (!archives_[archive_count_].Open(path)) {
      continue;
    }
    // Trim the extension for display.
    char* dot = std::strrchr(base, '.');
    if (dot != nullptr) {
      *dot = '\0';
    }
    std::snprintf(archive_names_[archive_count_],
        kMapArchiveNameMaxLength, "%s", base);
    ++archive_count_;
  }
  closedir(handle);

  // A marked-trail overlay is optional; the map works without one.
  handle = opendir(directory);
  if (handle != nullptr) {
    while (true) {
      const dirent* entry = readdir(handle);
      if (entry == nullptr) {
        break;
      }
      const size_t length = std::strlen(entry->d_name);
      if (length <= 9 ||
          strncasecmp(entry->d_name + length - 9, ".svtrails", 9) != 0) {
        continue;
      }
      char path[kMaxPathLength] = {};
      if (std::snprintf(path, sizeof(path), "%s/%s", directory,
              entry->d_name) < static_cast<int>(sizeof(path)) &&
          trails_.Open(path)) {
        break;
      }
    }
    closedir(handle);
  }

  // Most detailed first. The archives stay put and only the lookup order is
  // sorted, because an open archive cannot be moved.
  for (size_t index = 0; index < archive_count_; ++index) {
    order_[index] = index;
  }
  for (size_t index = 1; index < archive_count_; ++index) {
    for (size_t other = index; other > 0 &&
        archives_[order_[other]].max_zoom() >
            archives_[order_[other - 1]].max_zoom();
        --other) {
      const size_t swap = order_[other];
      order_[other] = order_[other - 1];
      order_[other - 1] = swap;
    }
  }

  if (archive_count_ > 0) {
    cache_ = static_cast<MapCachedTile*>(heap_caps_calloc(kCacheTiles,
        sizeof(MapCachedTile), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    cache_size_ = cache_ == nullptr ? 0 : kCacheTiles;
    LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
        "Map source: %u archive(s), max zoom %u\n",
        static_cast<unsigned int>(archive_count_),
        static_cast<unsigned int>(max_zoom()));
  }
  return archive_count_;
}

void MapSource::ClearCache() {
  for (size_t index = 0; index < cache_size_; ++index) {
    cache_[index].valid = false;
  }
}

void MapSource::Close() {
  trails_.Close();
  for (size_t index = 0; index < cache_size_; ++index) {
    heap_caps_free(cache_[index].pixels);
  }
  heap_caps_free(cache_);
  cache_ = nullptr;
  cache_size_ = 0;
  for (size_t index = 0; index < archive_count_; ++index) {
    archives_[index].Close();
  }
  archive_count_ = 0;
  clock_ = 0;
}

bool MapSource::IsCached(uint8_t zoom, uint32_t x, uint32_t y) const {
  for (size_t index = 0; index < cache_size_; ++index) {
    const MapCachedTile& tile = cache_[index];
    if (tile.valid && tile.zoom == zoom && tile.x == x && tile.y == y) {
      return true;
    }
  }
  return false;
}

MapCachedTile* MapSource::AcquireSlot(uint8_t zoom, uint32_t x, uint32_t y) {
  MapCachedTile* oldest = nullptr;
  for (size_t index = 0; index < cache_size_; ++index) {
    MapCachedTile& tile = cache_[index];
    if (!tile.valid) {
      oldest = &tile;
      break;
    }
    if (oldest == nullptr || tile.last_used < oldest->last_used) {
      oldest = &tile;
    }
  }
  if (oldest == nullptr) {
    return nullptr;
  }
  if (oldest->pixels == nullptr) {
    oldest->pixels = static_cast<uint16_t*>(
        heap_caps_malloc(kTileBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (oldest->pixels == nullptr) {
      return nullptr;
    }
  }
  oldest->zoom = zoom;
  oldest->x = x;
  oldest->y = y;
  oldest->valid = false;
  return oldest;
}

bool MapSource::RenderInto(uint8_t zoom, uint32_t x, uint32_t y,
    uint16_t* pixels, bool* approximate) {
  *approximate = false;
  for (size_t position = 0; position < archive_count_; ++position) {
    PmTilesArchive& archive = archives_[order_[position]];
    if (zoom < archive.min_zoom()) {
      continue;
    }

    // Walk up the pyramid until this archive has a tile that covers the one
    // being asked for.
    for (uint8_t level = 0; level <= kMaxParentLevels; ++level) {
      if (level > zoom) {
        break;
      }
      const uint8_t source_zoom = static_cast<uint8_t>(zoom - level);
      if (source_zoom > archive.max_zoom()) {
        continue;
      }
      const uint32_t divisions = 1U << level;
      const uint32_t source_x = x >> level;
      const uint32_t source_y = y >> level;

      PmTile tile;
      if (!archive.ReadTile(source_zoom, source_x, source_y, &tile)) {
        continue;
      }
      MvtTile decoded;
      const bool ok = decoded.Decode(tile.data, tile.size);
      archive.FreeTile(&tile);
      if (!ok) {
        continue;
      }
      // The slice of the coarse tile that lines up with the requested one.
      RenderMapTile(decoded, style_, zoom, divisions,
          x - (source_x << level), y - (source_y << level), pixels);
      if (trails_enabled_ && trails_.IsOpen()) {
        trails_.DrawTile(zoom, x, y, night_style_, pixels);
      }
      *approximate = level > 0;
      return true;
    }
  }
  return false;
}

const uint16_t* MapSource::GetTile(
    uint8_t zoom, uint32_t x, uint32_t y, bool* approximate) {
  bool ignored = false;
  if (approximate == nullptr) {
    approximate = &ignored;
  }
  if (archive_count_ == 0 || cache_size_ == 0) {
    return nullptr;
  }

  ++clock_;
  for (size_t index = 0; index < cache_size_; ++index) {
    MapCachedTile& tile = cache_[index];
    if (tile.valid && tile.zoom == zoom && tile.x == x && tile.y == y) {
      tile.last_used = clock_;
      *approximate = tile.approximate;
      return tile.pixels;
    }
  }

  MapCachedTile* slot = AcquireSlot(zoom, x, y);
  if (slot == nullptr) {
    return nullptr;
  }
  if (!RenderInto(zoom, x, y, slot->pixels, approximate)) {
    // Nothing covers this tile, but the empty result is worth caching so the
    // same miss is not paid for on every frame.
    FillMapTileBackground(style_, slot->pixels);
    slot->approximate = true;
  } else {
    slot->approximate = *approximate;
  }
  slot->valid = true;
  slot->last_used = clock_;
  *approximate = slot->approximate;
  return slot->pixels;
}

}  // namespace lilygo_box::survival
