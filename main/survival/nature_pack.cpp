/**
 * @Description: Reader for the field guide pack (nature.svnat)
 * @License: GPL 3.0
 */
#include "survival/nature_pack.h"

#include <cctype>
#include <cstdlib>
#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"

namespace lilygo_box::survival {
namespace {

constexpr char kMagic[8] = {'S', 'V', 'N', 'A', 'T', '2', 0, 0};
constexpr size_t kHeaderSize = 72;
// Guards against a damaged header asking for absurd amounts of memory.
constexpr uint32_t kMaxSpecies = 200000;
constexpr uint32_t kMaxNamesBytes = 32 * 1024 * 1024;
constexpr uint32_t kMaxTextBytes = 256 * 1024;
constexpr uint32_t kMaxPhotoBytes = 4 * 1024 * 1024;

// Base letters of U+00C0..U+017F, so "Č" folds to "c" and "ů" to "u".
constexpr char kFoldLatin1[] =
    "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
constexpr char kFoldExtendedA[] =
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

void* AllocateLarge(size_t bytes) {
  void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return memory != nullptr ? memory : std::malloc(bytes);
}

}  // namespace

size_t FoldForSearch(const char* text, char* out, size_t size) {
  if (out == nullptr || size == 0) {
    return 0;
  }
  size_t length = 0;
  const auto* bytes = reinterpret_cast<const unsigned char*>(text != nullptr ? text : "");
  while (*bytes != 0 && length + 1 < size) {
    uint32_t code = *bytes;
    int extra = 0;
    if (code >= 0xF0) {
      code &= 0x07; extra = 3;
    } else if (code >= 0xE0) {
      code &= 0x0F; extra = 2;
    } else if (code >= 0xC0) {
      code &= 0x1F; extra = 1;
    }
    ++bytes;
    for (int i = 0; i < extra && (*bytes & 0xC0) == 0x80; ++i, ++bytes) {
      code = (code << 6) | (*bytes & 0x3F);
    }
    char folded = 0;
    if (code < 0x80) {
      folded = static_cast<char>(std::tolower(static_cast<int>(code)));
    } else if (code >= 0xC0 && code <= 0xFF) {
      folded = kFoldLatin1[code - 0xC0];
    } else if (code >= 0x100 && code <= 0x17F) {
      folded = kFoldExtendedA[code - 0x100];
    } else {
      folded = ' ';
    }
    out[length++] = static_cast<char>(std::tolower(static_cast<unsigned char>(folded)));
  }
  out[length] = '\0';
  return length;
}

NaturePack::~NaturePack() { Close(); }

void NaturePack::Close() {
  file_.Close();
  heap_caps_free(species_);
  heap_caps_free(photos_);
  heap_caps_free(names_);
  heap_caps_free(search_text_);
  heap_caps_free(search_offsets_);
  species_ = nullptr;
  photos_ = nullptr;
  names_ = nullptr;
  search_text_ = nullptr;
  search_offsets_ = nullptr;
  species_count_ = 0;
  photo_count_ = 0;
  names_bytes_ = 0;
}

bool NaturePack::Open(const char* path) {
  Close();
  if (!file_.Open(path)) {
    return false;
  }
  uint8_t header[kHeaderSize] = {};
  if (!file_.ReadAt(0, header, sizeof(header)) ||
      std::memcmp(header, kMagic, sizeof(kMagic)) != 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "%s is not a field guide pack\n", path);
    file_.Close();
    return false;
  }
  uint32_t counts[4] = {};
  uint64_t offsets[6] = {};
  std::memcpy(counts, header + 8, sizeof(counts));
  std::memcpy(offsets, header + 24, sizeof(offsets));
  const uint32_t species_count = counts[0];
  const uint32_t photo_count = counts[1];
  const uint32_t names_bytes = counts[2];
  if (species_count == 0 || species_count > kMaxSpecies ||
      photo_count > kMaxSpecies * 8 || names_bytes > kMaxNamesBytes) {
    file_.Close();
    return false;
  }
  species_ = static_cast<NatureSpecies*>(
      AllocateLarge(sizeof(NatureSpecies) * species_count));
  photos_ = static_cast<NaturePhoto*>(
      AllocateLarge(sizeof(NaturePhoto) * (photo_count + 1)));
  names_ = static_cast<char*>(AllocateLarge(names_bytes + 1));
  if (species_ == nullptr || photos_ == nullptr || names_ == nullptr ||
      !file_.ReadAt(offsets[0], species_, sizeof(NatureSpecies) * species_count) ||
      !file_.ReadAt(offsets[1], photos_, sizeof(NaturePhoto) * photo_count) ||
      !file_.ReadAt(offsets[2], names_, names_bytes)) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Reading the field guide tables failed\n");
    Close();
    return false;
  }
  names_[names_bytes] = '\0';
  species_count_ = species_count;
  photo_count_ = photo_count;
  names_bytes_ = names_bytes;
  texts_offset_ = offsets[3];
  texts_bytes_ = offsets[4];
  blob_offset_ = offsets[5];

  // One folded "czech|latin|english" line per species for searching.
  size_t search_bytes = 0;
  for (uint32_t i = 0; i < species_count_; ++i) {
    search_bytes += std::strlen(Name(species_[i].name_cs)) +
        std::strlen(Name(species_[i].name_lat)) +
        std::strlen(Name(species_[i].name_en)) + 3;
  }
  search_text_ = static_cast<char*>(AllocateLarge(search_bytes + 1));
  search_offsets_ = static_cast<uint32_t*>(
      AllocateLarge(sizeof(uint32_t) * species_count_));
  if (search_text_ == nullptr || search_offsets_ == nullptr) {
    Close();
    return false;
  }
  size_t used = 0;
  for (uint32_t i = 0; i < species_count_; ++i) {
    search_offsets_[i] = static_cast<uint32_t>(used);
    const char* parts[3] = {Name(species_[i].name_cs), Name(species_[i].name_lat),
        Name(species_[i].name_en)};
    for (int part = 0; part < 3; ++part) {
      used += FoldForSearch(parts[part], search_text_ + used, search_bytes + 1 - used);
      if (part < 2 && used < search_bytes) {
        search_text_[used++] = '|';
      }
    }
    if (used < search_bytes + 1) {
      search_text_[used++] = '\0';
    }
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "Field guide: %lu species, %lu photos\n",
      static_cast<unsigned long>(species_count_),
      static_cast<unsigned long>(photo_count_));
  return true;
}

const char* NaturePack::Name(uint32_t offset) const {
  if (names_ == nullptr || offset >= names_bytes_) {
    return "";
  }
  return names_ + offset;
}

const char* NaturePack::SearchKey(uint32_t index) const {
  if (search_text_ == nullptr || index >= species_count_) {
    return "";
  }
  return search_text_ + search_offsets_[index];
}

char* NaturePack::ReadText(const NatureSpecies& species) {
  const uint32_t length = species.text_length < kMaxTextBytes
      ? species.text_length : kMaxTextBytes;
  if (species.text + length > texts_bytes_) {
    return nullptr;
  }
  auto* text = static_cast<char*>(AllocateLarge(length + 1));
  if (text == nullptr) {
    return nullptr;
  }
  if (length > 0 && !file_.ReadAt(texts_offset_ + species.text, text, length)) {
    heap_caps_free(text);
    return nullptr;
  }
  text[length] = '\0';
  return text;
}

uint8_t* NaturePack::ReadPhoto(uint32_t index, size_t* size) {
  if (index >= photo_count_ || size == nullptr ||
      photos_[index].size == 0 || photos_[index].size > kMaxPhotoBytes) {
    return nullptr;
  }
  auto* data = static_cast<uint8_t*>(AllocateLarge(photos_[index].size));
  if (data == nullptr) {
    return nullptr;
  }
  if (!file_.ReadAt(blob_offset_ + photos_[index].offset, data, photos_[index].size)) {
    heap_caps_free(data);
    return nullptr;
  }
  *size = photos_[index].size;
  return data;
}

}  // namespace lilygo_box::survival
