/*
 * @Description: Reader for the world field guide (nature-world.svnat3)
 * @License: GPL 3.0
 *
 * Hundreds of thousands of species do not fit in memory, so only 8 bytes of
 * filter attributes per species stay in PSRAM; records, names, texts and
 * photos are read from the card when shown. Format: tools/prepare_world_nature.py.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "survival/split_file.h"

namespace lilygo_box::survival {

inline constexpr uint32_t kNatureNoString = 0xFFFFFFFF;

#pragma pack(push, 1)
struct NatureRecord {
  uint32_t inaturalist_id;
  uint32_t observations;
  uint32_t name_cs;
  uint32_t name_en;
  uint32_t name_lat;
  uint32_t family;
  uint32_t text;
  uint32_t text_length;
  uint32_t first_photo;
  uint32_t thumbnail;
  uint32_t text_credit;
  uint32_t reserved;
};
struct NatureAttributes {
  uint8_t group;
  uint8_t color;
  uint8_t uses;
  uint8_t regions;
  uint16_t months;
  uint8_t photo_count;
  uint8_t habitats;
};
struct NaturePhotoEntry {
  uint64_t offset;
  uint32_t size;
  uint32_t credit;
};
#pragma pack(pop)
static_assert(sizeof(NatureRecord) == 48, "record is 48 bytes");
static_assert(sizeof(NatureAttributes) == 8, "attributes are 8 bytes");
static_assert(sizeof(NaturePhotoEntry) == 16, "photo entry is 16 bytes");

// Values of NatureAttributes::regions.
enum NatureRegion : uint8_t {
  kRegionCzechia = 1,
  kRegionEurope = 2,
  kRegionAfrica = 4,
  kRegionNorthAmerica = 8,
  kRegionSouthAmerica = 16,
  kRegionOceania = 32,
  kRegionAsia = 64,
};

/**
 * @brief Folds a name or query for search: lower case, no accents, anything
 *        that is not a letter or digit becomes a space (as the packer does).
 */
size_t FoldNatureName(const char* text, char* out, size_t size);

/**
 * @brief Looks in a folder for the world field guide, whole or split
 *        (`nature-world.svnat3`, or `nature-world.svnat3aa`, `…ab` …).
 * @param directory Folder to look in.
 * @param path Set to the path to open (the base name for split parts).
 * @return true when found.
 */
bool FindNatureWorld(const char* directory, char* path, size_t size);

class NatureWorld {
 public:
  NatureWorld() = default;
  ~NatureWorld();
  NatureWorld(const NatureWorld&) = delete;
  NatureWorld& operator=(const NatureWorld&) = delete;

  /**
   * @brief Opens a pack (or its parts) and loads the attributes.
   */
  bool Open(const char* path);
  void Close();
  bool IsOpen() const { return attributes_ != nullptr; }

  uint32_t SpeciesCount() const { return species_count_; }
  const NatureAttributes& Attributes(uint32_t species) const { return attributes_[species]; }

  bool Record(uint32_t species, NatureRecord* out);
  /**
   * @brief A string from the pack; "" for kNatureNoString.
   * @return out.
   */
  const char* String(uint32_t offset, char* out, size_t size);
  bool Photo(uint32_t index, NaturePhotoEntry* out);
  /**
   * @brief Reads a photo's JPEG bytes into a PSRAM buffer (free with heap_caps_free).
   */
  uint8_t* ReadPhoto(uint32_t index, size_t* size);
  /**
   * @brief Reads a species' text into a PSRAM buffer (NUL-terminated).
   */
  char* ReadText(const NatureRecord& record);

  /**
   * @brief Species sorted by display name (Czech, else English, else Latin)
   *        or by Latin name; loaded on first use.
   * @return Array of SpeciesCount() indices, or nullptr.
   */
  const uint32_t* OrderByName();
  const uint32_t* OrderByLatin();

  /**
   * @brief Species whose Czech, English or Latin name has a word starting
   *        with the query (several words: the name continues with them).
   * @param query As typed.
   * @param out Species indices, most observed first, no duplicates.
   * @param capacity Size of out.
   * @return Number found.
   */
  int Search(const char* query, uint32_t* out, int capacity);

  /**
   * @brief Finds a species by its Latin name (exact, case-insensitive).
   * @return Species index, or -1.
   */
  int FindLatin(const char* latin);

 private:
  const uint32_t* LoadOrder(uint64_t offset, uint32_t** cache);
  int LowerBlock(const char* key) const;

  SplitFile file_;
  uint32_t species_count_ = 0;
  uint32_t photo_count_ = 0;
  uint64_t species_offset_ = 0;
  uint64_t photos_offset_ = 0;
  uint64_t strings_offset_ = 0;
  uint64_t strings_bytes_ = 0;
  uint64_t texts_offset_ = 0;
  uint64_t by_name_offset_ = 0;
  uint64_t by_latin_offset_ = 0;
  uint64_t search_offset_ = 0;
  uint32_t search_blocks_ = 0;
  NatureAttributes* attributes_ = nullptr;
  char* search_keys_ = nullptr;
  uint32_t* by_name_ = nullptr;
  uint32_t* by_latin_ = nullptr;
  uint8_t* block_ = nullptr;
};

}  // namespace lilygo_box::survival
