/**
 * @Description: Decoder for Mapbox Vector Tiles
 * @License: GPL 3.0
 */
#include "survival/mvt_decoder.h"

#include <algorithm>
#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"

namespace lilygo_box::survival {
namespace {

// Protobuf wire types.
constexpr uint32_t kWireVarint = 0;
constexpr uint32_t kWireFixed64 = 1;
constexpr uint32_t kWireLength = 2;
constexpr uint32_t kWireFixed32 = 5;

// Field numbers from the Mapbox Vector Tile specification.
constexpr uint32_t kTileLayer = 3;
constexpr uint32_t kLayerName = 1;
constexpr uint32_t kLayerFeature = 2;
constexpr uint32_t kLayerKey = 3;
constexpr uint32_t kLayerValue = 4;
constexpr uint32_t kLayerExtent = 5;
constexpr uint32_t kFeatureTags = 2;
constexpr uint32_t kFeatureType = 3;
constexpr uint32_t kFeatureGeometry = 4;
// Value message fields; only these two carry what the style needs.
constexpr uint32_t kValueString = 1;
constexpr uint32_t kValueInt = 4;

// A basemap layer names far fewer keys than this.
constexpr uint32_t kMaxKeys = 96;
constexpr uint32_t kMaxValues = 4096;

// Geometry commands.
constexpr uint32_t kCommandMoveTo = 1;
constexpr uint32_t kCommandLineTo = 2;
constexpr uint32_t kCommandClosePath = 7;

// A single tile that needs more than this is not one this device will draw.
constexpr uint32_t kMaxLayers = 24;
constexpr uint32_t kMaxFeatures = 8000;
constexpr uint32_t kMaxRings = 16000;
constexpr uint32_t kMaxPoints = 120000;

// A string that still points into the tile buffer.
struct StringRef {
  const char* data = nullptr;
  size_t size = 0;
};

// A protobuf cursor over a byte range.
struct Reader {
  const uint8_t* data = nullptr;
  size_t size = 0;
  size_t cursor = 0;
};

/**
 * @brief Reads one LEB128 varint.
 * @param reader Cursor to read from.
 * @param value Decoded value.
 * @return true when a complete varint was read.
 */
bool ReadVarint(Reader* reader, uint64_t* value) {
  uint64_t result = 0;
  int shift = 0;
  while (reader->cursor < reader->size && shift <= 63) {
    const uint8_t byte = reader->data[reader->cursor++];
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
 * @brief Reads a field tag and splits it into number and wire type.
 * @param reader Cursor to read from.
 * @param field Field number.
 * @param wire_type Wire type.
 * @return true when a tag was read.
 */
bool ReadTag(Reader* reader, uint32_t* field, uint32_t* wire_type) {
  uint64_t tag = 0;
  if (!ReadVarint(reader, &tag)) {
    return false;
  }
  *field = static_cast<uint32_t>(tag >> 3);
  *wire_type = static_cast<uint32_t>(tag & 0x07);
  return true;
}

/**
 * @brief Steps over a field the decoder does not care about.
 * @param reader Cursor to advance.
 * @param wire_type Wire type of the field.
 * @return true when the field was skipped.
 */
bool SkipField(Reader* reader, uint32_t wire_type) {
  switch (wire_type) {
    case kWireVarint: {
      uint64_t ignored = 0;
      return ReadVarint(reader, &ignored);
    }
    case kWireFixed64:
      reader->cursor += 8;
      return reader->cursor <= reader->size;
    case kWireLength: {
      uint64_t length = 0;
      if (!ReadVarint(reader, &length)) {
        return false;
      }
      reader->cursor += static_cast<size_t>(length);
      return reader->cursor <= reader->size;
    }
    case kWireFixed32:
      reader->cursor += 4;
      return reader->cursor <= reader->size;
    default:
      return false;
  }
}

/**
 * @brief Reads a length-delimited field as a sub-message cursor.
 * @param reader Cursor to read from.
 * @param nested Cursor over the nested bytes.
 * @return true when the nested range is valid.
 */
bool ReadNested(Reader* reader, Reader* nested) {
  uint64_t length = 0;
  if (!ReadVarint(reader, &length) ||
      reader->cursor + length > reader->size) {
    return false;
  }
  nested->data = reader->data + reader->cursor;
  nested->size = static_cast<size_t>(length);
  nested->cursor = 0;
  reader->cursor += static_cast<size_t>(length);
  return true;
}

/**
 * @brief Undoes protobuf zig-zag encoding.
 * @param value Encoded value.
 * @return The signed value.
 */
int32_t ZigZagDecode(uint32_t value) {
  return static_cast<int32_t>((value >> 1) ^ (~(value & 1) + 1));
}

/**
 * @brief Grows an array in PSRAM.
 * @param pointer Array to grow.
 * @param capacity Current capacity, updated on success.
 * @param needed Required element count.
 * @param limit Hard cap on the capacity.
 * @param element_size Size of one element.
 * @return true when the array can hold `needed` elements.
 */
bool EnsureCapacity(void** pointer, uint32_t* capacity, uint32_t needed,
    uint32_t limit, size_t element_size) {
  if (needed <= *capacity) {
    return true;
  }
  if (needed > limit) {
    return false;
  }
  uint32_t grown = *capacity == 0 ? 256 : *capacity;
  while (grown < needed) {
    grown *= 2;
  }
  grown = std::min(grown, limit);
  void* larger = heap_caps_realloc(*pointer,
      static_cast<size_t>(grown) * element_size,
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (larger == nullptr) {
    return false;
  }
  *pointer = larger;
  *capacity = grown;
  return true;
}

/**
 * @brief Compares a tile string against a literal.
 * @param text String from the tile.
 * @param literal Literal to compare with.
 * @return true when they are equal.
 */
bool StringEquals(const StringRef& text, const char* literal) {
  const size_t length = std::strlen(literal);
  return text.size == length && std::memcmp(text.data, literal, length) == 0;
}

}  // namespace

MvtTile::~MvtTile() { Clear(); }

int16_t MvtTile::InternKind(const char* name, size_t length) {
  if (name == nullptr || length == 0) {
    return -1;
  }
  const size_t copy = std::min(length, kMvtKindMaxLength - 1);
  for (uint32_t index = 0; index < kind_count_; ++index) {
    if (std::strncmp(kinds_[index], name, copy) == 0 &&
        kinds_[index][copy] == '\0') {
      return static_cast<int16_t>(index);
    }
  }
  if (kind_count_ >= kind_capacity_) {
    if (kind_count_ >= kMvtMaxKinds) {
      return -1;
    }
    const uint32_t grown = std::min(
        kind_capacity_ == 0 ? 64U : kind_capacity_ * 2, kMvtMaxKinds);
    void* larger = heap_caps_realloc(kinds_,
        static_cast<size_t>(grown) * kMvtKindMaxLength,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (larger == nullptr) {
      return -1;
    }
    kinds_ = static_cast<char (*)[kMvtKindMaxLength]>(larger);
    kind_capacity_ = grown;
  }
  std::memcpy(kinds_[kind_count_], name, copy);
  kinds_[kind_count_][copy] = '\0';
  return static_cast<int16_t>(kind_count_++);
}

const char* MvtTile::kind_name(int16_t kind) const {
  if (kinds_ == nullptr || kind < 0 ||
      static_cast<uint32_t>(kind) >= kind_count_) {
    return "";
  }
  return kinds_[kind];
}

void MvtTile::Clear() {
  heap_caps_free(kinds_);
  kinds_ = nullptr;
  kind_capacity_ = 0;
  heap_caps_free(layers_);
  heap_caps_free(features_);
  heap_caps_free(rings_);
  heap_caps_free(points_);
  layers_ = nullptr;
  features_ = nullptr;
  rings_ = nullptr;
  points_ = nullptr;
  layer_count_ = 0;
  feature_count_ = 0;
  ring_count_ = 0;
  point_count_ = 0;
  feature_capacity_ = 0;
  ring_capacity_ = 0;
  point_capacity_ = 0;
  kind_count_ = 0;
}

const MvtLayer* MvtTile::layer(uint32_t index) const {
  return index < layer_count_ ? &layers_[index] : nullptr;
}

const MvtFeature* MvtTile::feature(uint32_t index) const {
  return index < feature_count_ ? &features_[index] : nullptr;
}

const MvtRing* MvtTile::ring(uint32_t index) const {
  return index < ring_count_ ? &rings_[index] : nullptr;
}

const MvtPoint* MvtTile::points(const MvtRing& ring) const {
  return points_ == nullptr ? nullptr : points_ + ring.first_point;
}

const MvtLayer* MvtTile::FindLayer(const char* name) const {
  if (name == nullptr) {
    return nullptr;
  }
  for (uint32_t index = 0; index < layer_count_; ++index) {
    if (std::strcmp(layers_[index].name, name) == 0) {
      return &layers_[index];
    }
  }
  return nullptr;
}

bool MvtTile::Decode(const uint8_t* data, size_t size) {
  Clear();
  if (data == nullptr || size == 0) {
    return false;
  }

  layers_ = static_cast<MvtLayer*>(heap_caps_calloc(
      kMaxLayers, sizeof(MvtLayer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (layers_ == nullptr) {
    return false;
  }

  Reader tile = {data, size, 0};
  while (tile.cursor < tile.size && layer_count_ < kMaxLayers) {
    uint32_t field = 0;
    uint32_t wire_type = 0;
    if (!ReadTag(&tile, &field, &wire_type)) {
      break;
    }
    if (field != kTileLayer || wire_type != kWireLength) {
      if (!SkipField(&tile, wire_type)) {
        break;
      }
      continue;
    }

    Reader layer_reader;
    if (!ReadNested(&tile, &layer_reader)) {
      break;
    }

    MvtLayer& layer = layers_[layer_count_];
    layer.first_feature = feature_count_;
    layer.extent = 4096;

    // Features are decoded in a second pass, because the extent and the
    // key/value tables have to be known first and the specification does not
    // fix the field order.
    int32_t kind_key = -1;
    int32_t min_zoom_key = -1;
    uint32_t key_index = 0;
    StringRef values[kMaxValues];
    int64_t value_numbers[kMaxValues] = {};
    uint32_t value_count = 0;

    Reader scan = layer_reader;
    while (scan.cursor < scan.size) {
      uint32_t layer_field = 0;
      uint32_t layer_wire = 0;
      if (!ReadTag(&scan, &layer_field, &layer_wire)) {
        break;
      }
      if (layer_field == kLayerName && layer_wire == kWireLength) {
        Reader name;
        if (!ReadNested(&scan, &name)) {
          break;
        }
        const size_t copy = std::min(name.size, sizeof(layer.name) - 1);
        std::memcpy(layer.name, name.data, copy);
        layer.name[copy] = '\0';
        continue;
      }
      if (layer_field == kLayerExtent && layer_wire == kWireVarint) {
        uint64_t extent = 0;
        if (!ReadVarint(&scan, &extent)) {
          break;
        }
        if (extent > 0) {
          layer.extent = static_cast<uint32_t>(extent);
        }
        continue;
      }
      if (layer_field == kLayerKey && layer_wire == kWireLength) {
        Reader key;
        if (!ReadNested(&scan, &key)) {
          break;
        }
        const StringRef text = {
            reinterpret_cast<const char*>(key.data), key.size};
        if (StringEquals(text, "kind")) {
          kind_key = static_cast<int32_t>(key_index);
        } else if (StringEquals(text, "min_zoom")) {
          min_zoom_key = static_cast<int32_t>(key_index);
        }
        if (key_index < kMaxKeys) {
          ++key_index;
        }
        continue;
      }
      if (layer_field == kLayerValue && layer_wire == kWireLength) {
        Reader value;
        if (!ReadNested(&scan, &value)) {
          break;
        }
        if (value_count < kMaxValues) {
          while (value.cursor < value.size) {
            uint32_t value_field = 0;
            uint32_t value_wire = 0;
            if (!ReadTag(&value, &value_field, &value_wire)) {
              break;
            }
            if (value_field == kValueString && value_wire == kWireLength) {
              Reader text;
              if (!ReadNested(&value, &text)) {
                break;
              }
              values[value_count] = {
                  reinterpret_cast<const char*>(text.data), text.size};
              continue;
            }
            if (value_field == kValueInt && value_wire == kWireVarint) {
              uint64_t number = 0;
              if (!ReadVarint(&value, &number)) {
                break;
              }
              value_numbers[value_count] = static_cast<int64_t>(number);
              continue;
            }
            if (!SkipField(&value, value_wire)) {
              break;
            }
          }
          ++value_count;
        }
        continue;
      }
      if (!SkipField(&scan, layer_wire)) {
        break;
      }
    }

    layer_reader.cursor = 0;
    while (layer_reader.cursor < layer_reader.size) {
      uint32_t layer_field = 0;
      uint32_t layer_wire = 0;
      if (!ReadTag(&layer_reader, &layer_field, &layer_wire)) {
        break;
      }
      if (layer_field != kLayerFeature || layer_wire != kWireLength) {
        if (!SkipField(&layer_reader, layer_wire)) {
          break;
        }
        continue;
      }

      Reader feature_reader;
      if (!ReadNested(&layer_reader, &feature_reader)) {
        break;
      }

      auto* features = reinterpret_cast<void*>(features_);
      if (!EnsureCapacity(&features, &feature_capacity_, feature_count_ + 1,
              kMaxFeatures, sizeof(MvtFeature))) {
        break;
      }
      features_ = static_cast<MvtFeature*>(features);

      MvtFeature& feature = features_[feature_count_];
      feature = MvtFeature();
      feature.first_ring = ring_count_;

      Reader geometry;
      geometry.data = nullptr;
      while (feature_reader.cursor < feature_reader.size) {
        uint32_t feature_field = 0;
        uint32_t feature_wire = 0;
        if (!ReadTag(&feature_reader, &feature_field, &feature_wire)) {
          break;
        }
        if (feature_field == kFeatureTags && feature_wire == kWireLength) {
          // Tags are a packed run of key/value index pairs.
          Reader tags;
          if (!ReadNested(&feature_reader, &tags)) {
            break;
          }
          while (tags.cursor < tags.size) {
            uint64_t key = 0;
            uint64_t value = 0;
            if (!ReadVarint(&tags, &key) || !ReadVarint(&tags, &value)) {
              break;
            }
            if (value >= value_count) {
              continue;
            }
            if (kind_key >= 0 && key == static_cast<uint64_t>(kind_key) &&
                values[value].data != nullptr) {
              feature.kind =
                  InternKind(values[value].data, values[value].size);
            } else if (min_zoom_key >= 0 &&
                key == static_cast<uint64_t>(min_zoom_key)) {
              const int64_t zoom = value_numbers[value];
              if (zoom > 0 && zoom < 32) {
                feature.min_zoom = static_cast<uint8_t>(zoom);
              }
            }
          }
          continue;
        }
        if (feature_field == kFeatureType && feature_wire == kWireVarint) {
          uint64_t type = 0;
          if (!ReadVarint(&feature_reader, &type)) {
            break;
          }
          feature.type = static_cast<MvtGeometryType>(type);
          continue;
        }
        if (feature_field == kFeatureGeometry &&
            feature_wire == kWireLength) {
          if (!ReadNested(&feature_reader, &geometry)) {
            break;
          }
          continue;
        }
        if (!SkipField(&feature_reader, feature_wire)) {
          break;
        }
      }

      if (geometry.data != nullptr) {
        // Geometry is a command stream: MoveTo starts a ring, LineTo extends
        // it, ClosePath ends a polygon ring.
        int32_t cursor_x = 0;
        int32_t cursor_y = 0;
        bool ring_open = false;
        uint32_t ring_first_point = 0;

        auto close_ring = [&]() {
          if (!ring_open) {
            return true;
          }
          ring_open = false;
          if (point_count_ <= ring_first_point) {
            return true;
          }
          auto* rings = reinterpret_cast<void*>(rings_);
          if (!EnsureCapacity(&rings, &ring_capacity_, ring_count_ + 1,
                  kMaxRings, sizeof(MvtRing))) {
            return false;
          }
          rings_ = static_cast<MvtRing*>(rings);
          rings_[ring_count_].first_point = ring_first_point;
          rings_[ring_count_].point_count = point_count_ - ring_first_point;
          ++ring_count_;
          ++feature.ring_count;
          return true;
        };

        bool geometry_ok = true;
        while (geometry.cursor < geometry.size && geometry_ok) {
          uint64_t command = 0;
          if (!ReadVarint(&geometry, &command)) {
            break;
          }
          const uint32_t operation = static_cast<uint32_t>(command & 0x07);
          const uint32_t repeat = static_cast<uint32_t>(command >> 3);

          if (operation == kCommandClosePath) {
            geometry_ok = close_ring();
            continue;
          }
          if (operation != kCommandMoveTo && operation != kCommandLineTo) {
            break;
          }

          for (uint32_t step = 0; step < repeat && geometry_ok; ++step) {
            uint64_t raw_x = 0;
            uint64_t raw_y = 0;
            if (!ReadVarint(&geometry, &raw_x) ||
                !ReadVarint(&geometry, &raw_y)) {
              geometry_ok = false;
              break;
            }
            cursor_x += ZigZagDecode(static_cast<uint32_t>(raw_x));
            cursor_y += ZigZagDecode(static_cast<uint32_t>(raw_y));

            if (operation == kCommandMoveTo) {
              // A MoveTo always starts a new ring or line.
              if (!close_ring()) {
                geometry_ok = false;
                break;
              }
              ring_first_point = point_count_;
              ring_open = true;
            }

            auto* points = reinterpret_cast<void*>(points_);
            if (!EnsureCapacity(&points, &point_capacity_, point_count_ + 1,
                    kMaxPoints, sizeof(MvtPoint))) {
              geometry_ok = false;
              break;
            }
            points_ = static_cast<MvtPoint*>(points);
            // Coordinates outside the tile are clamped: the renderer clips to
            // the tile anyway and this keeps them in sixteen bits.
            points_[point_count_].x = static_cast<int16_t>(
                std::clamp<int32_t>(cursor_x, -32000, 32000));
            points_[point_count_].y = static_cast<int16_t>(
                std::clamp<int32_t>(cursor_y, -32000, 32000));
            ++point_count_;
          }
        }
        close_ring();
      }

      if (feature.ring_count > 0) {
        ++feature_count_;
      }
    }

    layer.feature_count = feature_count_ - layer.first_feature;
    if (layer.feature_count > 0 || layer.name[0] != '\0') {
      ++layer_count_;
    }
  }

  if (layer_count_ == 0) {
    Clear();
    return false;
  }
  return true;
}

}  // namespace lilygo_box::survival
