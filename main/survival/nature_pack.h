/**
 * @Description: Reader for the field guide pack (nature.svnat)
 * @License: GPL 3.0
 *
 * The pack is built on a computer by tools/prepare_nature.py: every plant and
 * fungus recorded in Czechia with names, photos, uses, habitats and months.
 * The species table and the short strings are loaded into PSRAM for listing
 * and searching; article texts and photos are read from the card on demand.
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "survival/split_file.h"

namespace lilygo_box::survival {

#pragma pack(push, 1)
struct NatureSpecies {
  uint32_t id;
  uint32_t observations;
  uint32_t name_cs;
  uint32_t name_lat;
  uint32_t name_en;
  uint32_t family;
  uint64_t text;
  uint32_t text_length;
  uint32_t credit;
  uint32_t first_photo;
  uint8_t photo_count;
  uint8_t group;
  uint8_t color;
  uint8_t uses;
  uint16_t habitats;
  uint16_t months;
  uint32_t reserved[3];
};
struct NaturePhoto {
  uint64_t offset;
  uint32_t size;
  uint32_t credit;
};
#pragma pack(pop)
static_assert(sizeof(NatureSpecies) == 64, "pack record is 64 bytes");
static_assert(sizeof(NaturePhoto) == 16, "pack photo entry is 16 bytes");

// Values of NatureSpecies::group, in the order the pack writes them.
enum class NatureGroup : uint8_t {
  kFlowering, kTree, kFern, kConifer, kMoss, kMushroom, kLichen, kFungus,
};

// Bits of NatureSpecies::uses.
enum NatureUse : uint8_t {
  kNatureEdible = 1, kNaturePoisonous = 2, kNatureDeadly = 4, kNatureMedicinal = 8,
  kNatureInedible = 16, kNaturePsychoactive = 32, kNatureInvasive = 64,
  kNatureProtected = 128,
};

// Bits of NatureSpecies::habitats.
enum NatureHabitat : uint16_t {
  kNatureForest = 1, kNatureMeadow = 2, kNatureWater = 4, kNatureRocks = 8,
  kNatureFields = 16, kNatureGardens = 32, kNatureWood = 64, kNatureMountains = 128,
};

// Values of NatureSpecies::color; 255 means unknown.
inline constexpr int kNatureColorCount = 10;
inline constexpr uint8_t kNatureColorUnknown = 255;

class NaturePack {
 public:
  NaturePack() = default;
  ~NaturePack();
  NaturePack(const NaturePack&) = delete;
  NaturePack& operator=(const NaturePack&) = delete;

  /**
   * @brief Opens a pack and loads its tables.
   * @param path Path of the .svnat file (or the base name of split parts).
   * @return true when the pack is usable.
   */
  bool Open(const char* path);

  /**
   * @brief Releases the tables and closes the file.
   */
  void Close();

  bool IsOpen() const { return species_ != nullptr; }
  uint32_t SpeciesCount() const { return species_count_; }
  const NatureSpecies& Species(uint32_t index) const { return species_[index]; }
  const NaturePhoto& Photo(uint32_t index) const { return photos_[index]; }
  uint32_t PhotoCount() const { return photo_count_; }

  /**
   * @brief A string from the name area (names, families, credits).
   * @param offset Offset stored in a record.
   * @return The string, or "" when the offset is out of range.
   */
  const char* Name(uint32_t offset) const;

  /**
   * @brief Folded (lower-case, no diacritics) Czech, Latin and English names
   *        of one species joined by '|', for searching.
   * @param index Species index.
   * @return The folded text.
   */
  const char* SearchKey(uint32_t index) const;

  /**
   * @brief Reads a species' article text.
   * @param species Record.
   * @return A NUL-terminated buffer to release with free(), or nullptr.
   */
  char* ReadText(const NatureSpecies& species);

  /**
   * @brief Reads one photo's JPEG bytes.
   * @param index Photo index.
   * @param size Set to the number of bytes.
   * @return A buffer to release with free(), or nullptr.
   */
  uint8_t* ReadPhoto(uint32_t index, size_t* size);

 private:
  SplitFile file_;
  uint32_t species_count_ = 0;
  uint32_t photo_count_ = 0;
  uint32_t names_bytes_ = 0;
  uint64_t texts_offset_ = 0;
  uint64_t texts_bytes_ = 0;
  uint64_t blob_offset_ = 0;
  NatureSpecies* species_ = nullptr;
  NaturePhoto* photos_ = nullptr;
  char* names_ = nullptr;
  char* search_text_ = nullptr;
  uint32_t* search_offsets_ = nullptr;
};

/**
 * @brief Lower-cases a UTF-8 string and replaces accented Latin letters with
 *        their base letter ("Hřib" -> "hrib"), for diacritics-blind search.
 * @param text Input.
 * @param out Output buffer.
 * @param size Output size.
 * @return Length written, without the terminator.
 */
size_t FoldForSearch(const char* text, char* out, size_t size);

}  // namespace lilygo_box::survival
