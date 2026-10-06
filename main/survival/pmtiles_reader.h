/**
 * @Description: Reader for PMTiles v3 tile archives
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "survival/split_file.h"

namespace lilygo_box::survival {

// What the archive holds, from the header's tile type field.
enum class PmTileType : uint8_t {
  kUnknown = 0,
  kMvt = 1,
  kPng = 2,
  kJpeg = 3,
  kWebp = 4,
  kAvif = 5,
};

// A tile payload, already decompressed. Release with PmTilesArchive::FreeTile().
struct PmTile {
  uint8_t* data = nullptr;
  size_t size = 0;
};

// Reads PMTiles v3 archives, including ones split across FAT32-sized parts.
//
// PMTiles keeps a small root directory plus a pool of leaf directories, both
// gzip compressed, so a tile lookup is one search in memory and at most one
// extra read from the card.
class PmTilesArchive {
 public:
  PmTilesArchive() = default;
  ~PmTilesArchive();

  PmTilesArchive(const PmTilesArchive&) = delete;
  PmTilesArchive& operator=(const PmTilesArchive&) = delete;

  /**
   * @brief Opens an archive and loads its root directory.
   * @param path Path to the .pmtiles file, or to the base name of its parts.
   * @return true when the archive is usable.
   */
  bool Open(const char* path);

  /**
   * @brief Closes the archive and releases its caches.
   */
  void Close();

  /**
   * @brief Reports whether an archive is open.
   * @return true when open.
   */
  bool IsOpen() const { return file_.IsOpen(); }

  /**
   * @brief Lowest zoom level the archive covers.
   * @return Minimum zoom.
   */
  uint8_t min_zoom() const { return min_zoom_; }

  /**
   * @brief Highest zoom level the archive covers.
   * @return Maximum zoom.
   */
  uint8_t max_zoom() const { return max_zoom_; }

  /**
   * @brief What kind of tiles the archive stores.
   * @return Tile type.
   */
  PmTileType tile_type() const { return tile_type_; }

  /**
   * @brief Reads one tile.
   * @param zoom Zoom level.
   * @param x Tile column.
   * @param y Tile row, with the origin at the top left.
   * @param tile Tile output; the caller frees it with FreeTile().
   * @return true when the tile exists and was read.
   */
  bool ReadTile(uint8_t zoom, uint32_t x, uint32_t y, PmTile* tile);

  /**
   * @brief Releases a tile returned by ReadTile().
   * @param tile Tile to free.
   */
  void FreeTile(PmTile* tile);

  /**
   * @brief Converts tile coordinates to the archive's Hilbert tile id.
   * @param zoom Zoom level.
   * @param x Tile column.
   * @param y Tile row.
   * @return The tile id.
   */
  static uint64_t TileId(uint8_t zoom, uint32_t x, uint32_t y);

 private:
  // One decoded directory entry.
  struct Entry {
    uint64_t tile_id = 0;
    uint64_t offset = 0;
    uint32_t length = 0;
    uint32_t run_length = 0;
  };

  // A decoded directory held in memory.
  struct Directory {
    Entry* entries = nullptr;
    uint32_t count = 0;
  };

  /**
   * @brief Reads and decodes a directory from the archive.
   * @param offset Offset of the compressed directory.
   * @param length Compressed length.
   * @param directory Directory output.
   * @return true when the directory was decoded.
   */
  bool LoadDirectory(uint64_t offset, uint32_t length, Directory* directory);

  /**
   * @brief Releases a decoded directory.
   * @param directory Directory to free.
   */
  void FreeDirectory(Directory* directory);

  /**
   * @brief Finds the entry that covers a tile id.
   * @param directory Directory to search.
   * @param tile_id Tile id to find.
   * @param entry Matching entry output.
   * @return true when an entry covers the id.
   */
  static bool FindEntry(
      const Directory& directory, uint64_t tile_id, Entry* entry);

  SplitFile file_;

  uint64_t root_offset_ = 0;
  uint32_t root_length_ = 0;
  uint64_t leaf_offset_ = 0;
  uint64_t data_offset_ = 0;
  uint8_t internal_compression_ = 0;
  uint8_t tile_compression_ = 0;
  PmTileType tile_type_ = PmTileType::kUnknown;
  uint8_t min_zoom_ = 0;
  uint8_t max_zoom_ = 0;

  // The root directory is small and hit on every lookup, so it stays resident.
  Directory root_;
  // Neighbouring tiles almost always share a leaf directory.
  Directory leaf_;
  uint64_t leaf_cache_offset_ = UINT64_MAX;
};

}  // namespace lilygo_box::survival
