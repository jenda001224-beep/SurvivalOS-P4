/**
 * @Description: Reader for PMTiles v3 tile archives
 * @License: GPL 3.0
 */
#include "survival/pmtiles_reader.h"

#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "survival/inflate.h"

namespace lilygo_box::survival {
namespace {

constexpr size_t kHeaderBytes = 127;
constexpr char kMagic[] = "PMTiles";
constexpr uint8_t kSupportedVersion = 3;

// Compression identifiers from the PMTiles specification.
constexpr uint8_t kCompressionNone = 1;
constexpr uint8_t kCompressionGzip = 2;

// A directory bigger than this is not something worth loading on this device;
// the planet basemap's root directory is about 15 kB.
constexpr uint32_t kMaxDirectoryBytes = 4 * 1024 * 1024;
constexpr uint32_t kMaxDirectoryEntries = 200000;

/**
 * @brief Reads one LEB128 varint.
 * @param data Buffer to read from.
 * @param size Bytes available.
 * @param cursor Read position, advanced past the varint.
 * @param value Decoded value.
 * @return true when a complete varint was read.
 */
bool ReadVarint(
    const uint8_t* data, size_t size, size_t* cursor, uint64_t* value) {
  uint64_t result = 0;
  int shift = 0;
  while (*cursor < size && shift <= 63) {
    const uint8_t byte = data[(*cursor)++];
    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      *value = result;
      return true;
    }
    shift += 7;
  }
  return false;
}

/**
 * @brief Reads a little-endian 64-bit value from a buffer.
 * @param data Buffer to read from.
 * @return The value.
 */
uint64_t ReadUint64(const uint8_t* data) {
  uint64_t value = 0;
  for (int index = 7; index >= 0; --index) {
    value = (value << 8) | data[index];
  }
  return value;
}

}  // namespace

PmTilesArchive::~PmTilesArchive() { Close(); }

uint64_t PmTilesArchive::TileId(uint8_t zoom, uint32_t x, uint32_t y) {
  // Tiles are numbered along a Hilbert curve, with each zoom level laid out
  // after all the levels below it.
  uint64_t accumulator = 0;
  for (uint8_t level = 0; level < zoom; ++level) {
    accumulator += (1ULL << level) * (1ULL << level);
  }

  uint64_t position = 0;
  uint64_t tile_x = x;
  uint64_t tile_y = y;
  for (uint64_t step = 1ULL << (zoom == 0 ? 0 : zoom - 1); step > 0;
      step /= 2) {
    const uint64_t rx = (tile_x & step) > 0 ? 1 : 0;
    const uint64_t ry = (tile_y & step) > 0 ? 1 : 0;
    position += step * step * ((3 * rx) ^ ry);
    // Rotate the quadrant so the curve stays continuous.
    if (ry == 0) {
      if (rx == 1) {
        tile_x = step - 1 - tile_x;
        tile_y = step - 1 - tile_y;
      }
      const uint64_t swap = tile_x;
      tile_x = tile_y;
      tile_y = swap;
    }
  }
  return accumulator + position;
}

bool PmTilesArchive::LoadDirectory(
    uint64_t offset, uint32_t length, Directory* directory) {
  FreeDirectory(directory);
  if (length == 0 || length > kMaxDirectoryBytes) {
    return false;
  }

  auto* raw = static_cast<uint8_t*>(
      heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (raw == nullptr) {
    return false;
  }
  if (!file_.ReadAt(offset, raw, length)) {
    heap_caps_free(raw);
    return false;
  }

  uint8_t* plain = raw;
  size_t plain_size = length;
  bool owns_plain = false;
  if (internal_compression_ == kCompressionGzip) {
    uint8_t* inflated = nullptr;
    size_t inflated_size = 0;
    if (!InflateToBuffer(raw, length, &inflated, &inflated_size, length * 6)) {
      heap_caps_free(raw);
      return false;
    }
    plain = inflated;
    plain_size = inflated_size;
    owns_plain = true;
    heap_caps_free(raw);
  } else if (internal_compression_ != kCompressionNone) {
    heap_caps_free(raw);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Unsupported PMTiles directory compression %u\n",
        static_cast<unsigned int>(internal_compression_));
    return false;
  }

  size_t cursor = 0;
  uint64_t count = 0;
  if (!ReadVarint(plain, plain_size, &cursor, &count) || count == 0 ||
      count > kMaxDirectoryEntries) {
    if (owns_plain) {
      heap_caps_free(plain);
    } else {
      heap_caps_free(raw);
    }
    return false;
  }

  auto* entries = static_cast<Entry*>(heap_caps_calloc(
      static_cast<size_t>(count), sizeof(Entry),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (entries == nullptr) {
    if (owns_plain) {
      heap_caps_free(plain);
    } else {
      heap_caps_free(raw);
    }
    return false;
  }

  // The four fields are stored as four consecutive columns, not interleaved.
  bool ok = true;
  uint64_t previous_id = 0;
  for (uint64_t index = 0; index < count && ok; ++index) {
    uint64_t delta = 0;
    ok = ReadVarint(plain, plain_size, &cursor, &delta);
    previous_id += delta;
    entries[index].tile_id = previous_id;
  }
  for (uint64_t index = 0; index < count && ok; ++index) {
    uint64_t value = 0;
    ok = ReadVarint(plain, plain_size, &cursor, &value);
    entries[index].run_length = static_cast<uint32_t>(value);
  }
  for (uint64_t index = 0; index < count && ok; ++index) {
    uint64_t value = 0;
    ok = ReadVarint(plain, plain_size, &cursor, &value);
    entries[index].length = static_cast<uint32_t>(value);
  }
  for (uint64_t index = 0; index < count && ok; ++index) {
    uint64_t value = 0;
    ok = ReadVarint(plain, plain_size, &cursor, &value);
    // Zero means this blob sits immediately after the previous one, which is
    // how a clustered archive stores most of its entries.
    if (value == 0) {
      entries[index].offset = index > 0
          ? entries[index - 1].offset + entries[index - 1].length
          : 0;
    } else {
      entries[index].offset = value - 1;
    }
  }

  if (owns_plain) {
    heap_caps_free(plain);
  } else {
    heap_caps_free(raw);
  }
  if (!ok) {
    heap_caps_free(entries);
    return false;
  }

  directory->entries = entries;
  directory->count = static_cast<uint32_t>(count);
  return true;
}

void PmTilesArchive::FreeDirectory(Directory* directory) {
  if (directory == nullptr) {
    return;
  }
  heap_caps_free(directory->entries);
  directory->entries = nullptr;
  directory->count = 0;
}

bool PmTilesArchive::FindEntry(
    const Directory& directory, uint64_t tile_id, Entry* entry) {
  if (directory.entries == nullptr || directory.count == 0) {
    return false;
  }
  // Find the last entry whose id is at or below the target.
  uint32_t low = 0;
  uint32_t high = directory.count - 1;
  uint32_t best = UINT32_MAX;
  while (low <= high) {
    const uint32_t middle = low + (high - low) / 2;
    if (directory.entries[middle].tile_id <= tile_id) {
      best = middle;
      low = middle + 1;
    } else {
      if (middle == 0) {
        break;
      }
      high = middle - 1;
    }
  }
  if (best == UINT32_MAX) {
    return false;
  }
  *entry = directory.entries[best];
  return true;
}

bool PmTilesArchive::Open(const char* path) {
  Close();
  if (!file_.Open(path)) {
    return false;
  }

  uint8_t header[kHeaderBytes] = {};
  if (!file_.ReadAt(0, header, sizeof(header))) {
    Close();
    return false;
  }
  if (std::memcmp(header, kMagic, 7) != 0 || header[7] != kSupportedVersion) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Not a PMTiles v3 archive\n");
    Close();
    return false;
  }

  root_offset_ = ReadUint64(header + 8);
  root_length_ = static_cast<uint32_t>(ReadUint64(header + 16));
  leaf_offset_ = ReadUint64(header + 40);
  data_offset_ = ReadUint64(header + 56);
  internal_compression_ = header[97];
  tile_compression_ = header[98];
  tile_type_ = static_cast<PmTileType>(header[99]);
  min_zoom_ = header[100];
  max_zoom_ = header[101];

  if (!LoadDirectory(root_offset_, root_length_, &root_)) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "PMTiles root directory failed to load\n");
    Close();
    return false;
  }

  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "PMTiles: zoom %u..%u, type %u, root entries %u\n",
      static_cast<unsigned int>(min_zoom_),
      static_cast<unsigned int>(max_zoom_),
      static_cast<unsigned int>(tile_type_),
      static_cast<unsigned int>(root_.count));
  return true;
}

void PmTilesArchive::Close() {
  FreeDirectory(&root_);
  FreeDirectory(&leaf_);
  leaf_cache_offset_ = UINT64_MAX;
  file_.Close();
}

bool PmTilesArchive::ReadTile(
    uint8_t zoom, uint32_t x, uint32_t y, PmTile* tile) {
  if (tile == nullptr || !IsOpen() || zoom < min_zoom_ || zoom > max_zoom_) {
    return false;
  }
  const uint64_t tile_id = TileId(zoom, x, y);

  Entry entry;
  if (!FindEntry(root_, tile_id, &entry)) {
    return false;
  }

  // A run length of zero marks a pointer into the leaf directory pool. PMTiles
  // allows those to nest, but two levels is what the format ever produces.
  for (int level = 0; level < 3 && entry.run_length == 0; ++level) {
    const uint64_t offset = leaf_offset_ + entry.offset;
    if (offset != leaf_cache_offset_) {
      if (!LoadDirectory(offset, entry.length, &leaf_)) {
        return false;
      }
      leaf_cache_offset_ = offset;
    }
    if (!FindEntry(leaf_, tile_id, &entry)) {
      return false;
    }
  }
  if (entry.run_length == 0) {
    return false;
  }
  // A run covers consecutive tile ids that share one payload.
  if (tile_id < entry.tile_id || tile_id >= entry.tile_id + entry.run_length) {
    return false;
  }
  if (entry.length == 0) {
    return false;
  }

  auto* raw = static_cast<uint8_t*>(
      heap_caps_malloc(entry.length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (raw == nullptr) {
    return false;
  }
  if (!file_.ReadAt(data_offset_ + entry.offset, raw, entry.length)) {
    heap_caps_free(raw);
    return false;
  }

  if (tile_compression_ == kCompressionGzip) {
    uint8_t* plain = nullptr;
    size_t plain_size = 0;
    const bool inflated = InflateToBuffer(
        raw, entry.length, &plain, &plain_size, entry.length * 5);
    heap_caps_free(raw);
    if (!inflated) {
      return false;
    }
    tile->data = plain;
    tile->size = plain_size;
    return true;
  }
  if (tile_compression_ != kCompressionNone) {
    heap_caps_free(raw);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Unsupported PMTiles tile compression %u\n",
        static_cast<unsigned int>(tile_compression_));
    return false;
  }

  tile->data = raw;
  tile->size = entry.length;
  return true;
}

void PmTilesArchive::FreeTile(PmTile* tile) {
  if (tile == nullptr || tile->data == nullptr) {
    return;
  }
  heap_caps_free(tile->data);
  tile->data = nullptr;
  tile->size = 0;
}

}  // namespace lilygo_box::survival
