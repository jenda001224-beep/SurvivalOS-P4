/**
 * @Description: Decoder for Mapbox Vector Tiles
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace lilygo_box::survival {

inline constexpr size_t kMvtLayerNameMaxLength = 32;
inline constexpr size_t kMvtKindMaxLength = 24;
// A basemap tile carries well under a hundred distinct kinds even with
// thousands of points of interest, but the table grows on demand rather than
// being capped low: running out silently drops the styling of every layer
// decoded after the overflow.
inline constexpr uint32_t kMvtMaxKinds = 512;

// What a feature's geometry describes.
enum class MvtGeometryType : uint8_t {
  kUnknown = 0,
  kPoint = 1,
  kLine = 2,
  kPolygon = 3,
};

// One ring or line out of a feature, as tile-local coordinates.
struct MvtRing {
  // Index of the first point in the decoder's shared point buffer.
  uint32_t first_point = 0;
  uint32_t point_count = 0;
};

// One feature, already reduced to what the renderer needs.
struct MvtFeature {
  MvtGeometryType type = MvtGeometryType::kUnknown;
  uint32_t first_ring = 0;
  uint32_t ring_count = 0;
  // Index into the tile's interned `kind` strings, or -1 when the feature has
  // no kind tag. This is what a style keys off: `major_road`, `park`, `river`.
  int16_t kind = -1;
  // The zoom the basemap says this feature should first appear at. Drawing
  // everything regardless turns a city into a solid block of roads, so the
  // renderer honours it.
  uint8_t min_zoom = 0;
};

// One layer of a tile.
struct MvtLayer {
  char name[kMvtLayerNameMaxLength] = {};
  // Tile-local coordinate space, normally 4096.
  uint32_t extent = 4096;
  uint32_t first_feature = 0;
  uint32_t feature_count = 0;
};

// A point in tile-local coordinates.
struct MvtPoint {
  int16_t x = 0;
  int16_t y = 0;
};

// A decoded tile. All the geometry lives in flat arrays so the renderer can
// walk it without chasing pointers, and one allocation frees the lot.
class MvtTile {
 public:
  MvtTile() = default;
  ~MvtTile();

  MvtTile(const MvtTile&) = delete;
  MvtTile& operator=(const MvtTile&) = delete;

  /**
   * @brief Decodes a vector tile.
   * @param data Uncompressed MVT protobuf bytes.
   * @param size Number of bytes.
   * @return true when at least one layer was decoded.
   */
  bool Decode(const uint8_t* data, size_t size);

  /**
   * @brief Releases the decoded tile.
   */
  void Clear();

  /**
   * @brief Number of layers in the tile.
   * @return Layer count.
   */
  uint32_t layer_count() const { return layer_count_; }

  /**
   * @brief Reads one layer.
   * @param index Layer index.
   * @return The layer, or nullptr when the index is out of range.
   */
  const MvtLayer* layer(uint32_t index) const;

  /**
   * @brief Reads one feature.
   * @param index Feature index, from a layer's range.
   * @return The feature, or nullptr when the index is out of range.
   */
  const MvtFeature* feature(uint32_t index) const;

  /**
   * @brief Reads one ring.
   * @param index Ring index, from a feature's range.
   * @return The ring, or nullptr when the index is out of range.
   */
  const MvtRing* ring(uint32_t index) const;

  /**
   * @brief Reads the points of a ring.
   * @param ring Ring to read.
   * @return Pointer to the ring's first point.
   */
  const MvtPoint* points(const MvtRing& ring) const;

  /**
   * @brief Finds a layer by name.
   * @param name Layer name.
   * @return The layer, or nullptr when the tile has no such layer.
   */
  const MvtLayer* FindLayer(const char* name) const;

  /**
   * @brief Reads an interned `kind` string.
   * @param kind Index from a feature, or -1.
   * @return The string, or an empty string when the index is unset.
   */
  const char* kind_name(int16_t kind) const;

 private:
  MvtLayer* layers_ = nullptr;
  MvtFeature* features_ = nullptr;
  MvtRing* rings_ = nullptr;
  MvtPoint* points_ = nullptr;
  uint32_t layer_count_ = 0;
  uint32_t feature_count_ = 0;
  uint32_t ring_count_ = 0;
  uint32_t point_count_ = 0;
  uint32_t feature_capacity_ = 0;
  uint32_t ring_capacity_ = 0;
  uint32_t point_capacity_ = 0;

  /**
   * @brief Interns a `kind` string so features can reference it by index.
   * @param name String from the tile.
   * @param length Length of the string.
   * @return The index, or -1 when the table is full.
   */
  int16_t InternKind(const char* name, size_t length);

  // Distinct `kind` values seen in this tile, grown as they are met.
  char (*kinds_)[kMvtKindMaxLength] = nullptr;
  uint32_t kind_count_ = 0;
  uint32_t kind_capacity_ = 0;
};

}  // namespace lilygo_box::survival
