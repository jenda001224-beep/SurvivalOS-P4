/*
 * @Description: Reader for the world field guide (nature-world.svnat3)
 * @License: GPL 3.0
 */
#include "survival/nature_world.h"

#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "survival/nature_pack.h"

namespace lilygo_box::survival {
namespace {

constexpr char kMagic[8] = {'S', 'V', 'N', 'A', 'T', '3', 0, 0};
constexpr size_t kHeaderBytes = 256;
constexpr size_t kSearchBlock = 4096;
constexpr size_t kSearchKey = 32;
constexpr uint32_t kMaxSearchBlocks = 192;

uint32_t Le32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
      (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}
uint64_t Le64(const uint8_t* data) {
  return static_cast<uint64_t>(Le32(data)) | (static_cast<uint64_t>(Le32(data + 4)) << 32);
}

void* Psram(size_t bytes) {
  return heap_caps_malloc(bytes == 0 ? 1 : bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace

size_t FoldNatureName(const char* text, char* out, size_t size) {
  if (out == nullptr || size == 0) {
    return 0;
  }
  FoldForSearch(text, out, size);
  // Like the packer: anything that is not a letter or digit is a space, and
  // no space at either end.
  size_t length = 0;
  for (size_t i = 0; out[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(out[i]);
    out[length++] = std::isalnum(c) ? static_cast<char>(c) : ' ';
  }
  out[length] = '\0';
  size_t begin = 0;
  while (begin < length && out[begin] == ' ') ++begin;
  while (length > begin && out[length - 1] == ' ') --length;
  std::memmove(out, out + begin, length - begin);
  out[length - begin] = '\0';
  return length - begin;
}

bool FindNatureWorld(const char* directory, char* path, size_t size) {
  DIR* folder = directory != nullptr ? opendir(directory) : nullptr;
  if (folder == nullptr || path == nullptr || size == 0) {
    if (folder != nullptr) closedir(folder);
    return false;
  }
  bool found = false;
  for (dirent* entry = readdir(folder); entry != nullptr && !found; entry = readdir(folder)) {
    // macOS leaves "._name" shadow files on the card.
    if (entry->d_name[0] == '.') {
      continue;
    }
    const size_t length = std::strlen(entry->d_name);
    size_t keep = 0;
    if (length > 7 && strcasecmp(entry->d_name + length - 7, ".svnat3") == 0) {
      keep = length;
    } else if (length > 9 && strncasecmp(entry->d_name + length - 9, ".svnat3", 7) == 0 &&
               std::islower(static_cast<unsigned char>(entry->d_name[length - 2])) &&
               std::islower(static_cast<unsigned char>(entry->d_name[length - 1]))) {
      keep = length - 2;  // a split part: open the base name
    }
    if (keep > 0) {
      std::snprintf(path, size, "%s/%.*s", directory, static_cast<int>(keep), entry->d_name);
      found = true;
    }
  }
  closedir(folder);
  return found;
}

NatureWorld::~NatureWorld() { Close(); }

void NatureWorld::Close() {
  file_.Close();
  heap_caps_free(attributes_);
  heap_caps_free(search_keys_);
  heap_caps_free(by_name_);
  heap_caps_free(by_latin_);
  heap_caps_free(block_);
  attributes_ = nullptr;
  search_keys_ = nullptr;
  by_name_ = nullptr;
  by_latin_ = nullptr;
  block_ = nullptr;
  species_count_ = photo_count_ = search_blocks_ = 0;
}

bool NatureWorld::Open(const char* path) {
  Close();
  if (!file_.Open(path)) {
    return false;
  }
  uint8_t header[kHeaderBytes] = {};
  if (!file_.ReadAt(0, header, sizeof(header)) || std::memcmp(header, kMagic, 8) != 0 ||
      Le32(header + 8) != 1) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__, "Not a world field guide: %s\n", path);
    Close();
    return false;
  }
  species_count_ = Le32(header + 12);
  photo_count_ = Le32(header + 16);
  species_offset_ = Le64(header + 24);
  const uint64_t attributes_offset = Le64(header + 32);
  photos_offset_ = Le64(header + 40);
  strings_offset_ = Le64(header + 48);
  strings_bytes_ = Le64(header + 56);
  texts_offset_ = Le64(header + 64);
  by_name_offset_ = Le64(header + 80);
  by_latin_offset_ = Le64(header + 88);
  search_offset_ = Le64(header + 96);
  const uint64_t search_keys_offset = Le64(header + 104);
  search_blocks_ = Le32(header + 120);

  attributes_ = static_cast<NatureAttributes*>(Psram(sizeof(NatureAttributes) * species_count_));
  search_keys_ = static_cast<char*>(Psram(kSearchKey * search_blocks_));
  block_ = static_cast<uint8_t*>(Psram(kSearchBlock));
  if (attributes_ == nullptr || search_keys_ == nullptr || block_ == nullptr ||
      !file_.ReadAt(attributes_offset, attributes_, sizeof(NatureAttributes) * species_count_) ||
      !file_.ReadAt(search_keys_offset, search_keys_, kSearchKey * search_blocks_)) {
    Close();
    return false;
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__, "World field guide: %u species, %u photos\n",
      static_cast<unsigned>(species_count_), static_cast<unsigned>(photo_count_));
  return true;
}

bool NatureWorld::Record(uint32_t species, NatureRecord* out) {
  return species < species_count_ && out != nullptr &&
      file_.ReadAt(species_offset_ + static_cast<uint64_t>(species) * sizeof(NatureRecord), out,
          sizeof(NatureRecord));
}

const char* NatureWorld::String(uint32_t offset, char* out, size_t size) {
  if (out == nullptr || size == 0) {
    return "";
  }
  out[0] = '\0';
  if (offset == kNatureNoString || offset >= strings_bytes_) {
    return out;
  }
  const size_t take = static_cast<size_t>(std::min<uint64_t>(size - 1, strings_bytes_ - offset));
  if (!file_.ReadAt(strings_offset_ + offset, out, take)) {
    out[0] = '\0';
    return out;
  }
  out[take] = '\0';
  return out;
}

bool NatureWorld::Photo(uint32_t index, NaturePhotoEntry* out) {
  return index < photo_count_ && out != nullptr &&
      file_.ReadAt(photos_offset_ + static_cast<uint64_t>(index) * sizeof(NaturePhotoEntry), out,
          sizeof(NaturePhotoEntry));
}

uint8_t* NatureWorld::ReadPhoto(uint32_t index, size_t* size) {
  NaturePhotoEntry entry;
  if (!Photo(index, &entry) || entry.size == 0 || entry.size > 4 * 1024 * 1024) {
    return nullptr;
  }
  auto* data = static_cast<uint8_t*>(Psram(entry.size));
  if (data == nullptr || !file_.ReadAt(entry.offset, data, entry.size)) {
    heap_caps_free(data);
    return nullptr;
  }
  if (size != nullptr) *size = entry.size;
  return data;
}

char* NatureWorld::ReadText(const NatureRecord& record) {
  auto* text = static_cast<char*>(Psram(record.text_length + 1));
  if (text == nullptr) {
    return nullptr;
  }
  if (record.text_length > 0 && !file_.ReadAt(texts_offset_ + record.text, text, record.text_length)) {
    heap_caps_free(text);
    return nullptr;
  }
  text[record.text_length] = '\0';
  return text;
}

const uint32_t* NatureWorld::LoadOrder(uint64_t offset, uint32_t** cache) {
  if (*cache != nullptr) {
    return *cache;
  }
  auto* order = static_cast<uint32_t*>(Psram(sizeof(uint32_t) * species_count_));
  if (order == nullptr || !file_.ReadAt(offset, order, sizeof(uint32_t) * species_count_)) {
    heap_caps_free(order);
    return nullptr;
  }
  *cache = order;
  return order;
}

const uint32_t* NatureWorld::OrderByName() { return LoadOrder(by_name_offset_, &by_name_); }
const uint32_t* NatureWorld::OrderByLatin() { return LoadOrder(by_latin_offset_, &by_latin_); }

int NatureWorld::LowerBlock(const char* key) const {
  int low = 0;
  int high = static_cast<int>(search_blocks_) - 1;
  int found = 0;
  while (low <= high) {
    const int middle = (low + high) / 2;
    if (std::strncmp(search_keys_ + static_cast<size_t>(middle) * kSearchKey, key, kSearchKey) < 0) {
      found = middle;
      low = middle + 1;
    } else {
      high = middle - 1;
    }
  }
  return found;
}

int NatureWorld::Search(const char* query, uint32_t* out, int capacity) {
  if (out == nullptr || capacity <= 0 || search_blocks_ == 0) {
    return 0;
  }
  char key[96];
  const size_t key_length = FoldNatureName(query, key, sizeof(key));
  if (key_length == 0) {
    return 0;
  }
  int count = 0;
  bool past = false;
  // A two-letter query can match a large part of the index; past this many
  // blocks the list is long enough and the card would keep the search waiting.
  const uint32_t first = static_cast<uint32_t>(LowerBlock(key));
  const uint32_t last = std::min<uint32_t>(search_blocks_, first + kMaxSearchBlocks);
  for (uint32_t block = first; block < last && !past; ++block) {
    if (!file_.ReadAt(search_offset_ + static_cast<uint64_t>(block) * kSearchBlock, block_,
            kSearchBlock)) {
      break;
    }
    const uint16_t entries = static_cast<uint16_t>(block_[0] | (block_[1] << 8));
    size_t cursor = 2;
    for (uint16_t i = 0; i < entries && cursor < kSearchBlock; ++i) {
      const size_t length = block_[cursor];
      const char* entry = reinterpret_cast<const char*>(block_ + cursor + 1);
      const uint32_t species = Le32(block_ + cursor + 1 + length);
      cursor += 1 + length + 4;
      const int order = std::strncmp(entry, key, std::min(length, key_length));
      if (order < 0 || (order == 0 && length < key_length)) {
        continue;
      }
      if (order > 0) {
        past = true;
        break;
      }
      if (std::find(out, out + count, species) == out + count) {
        if (count < capacity) {
          out[count++] = species;
        } else {
          // Keep the most observed: replace the largest index if this is smaller.
          uint32_t* worst = std::max_element(out, out + count);
          if (species < *worst) *worst = species;
        }
      }
    }
  }
  std::sort(out, out + count);
  return count;
}

int NatureWorld::FindLatin(const char* latin) {
  const uint32_t* order = OrderByLatin();
  if (order == nullptr || latin == nullptr) {
    return -1;
  }
  char want[128];
  FoldNatureName(latin, want, sizeof(want));
  int low = 0;
  int high = static_cast<int>(species_count_) - 1;
  char name[160];
  char folded[160];
  while (low <= high) {
    const int middle = (low + high) / 2;
    NatureRecord record;
    if (!Record(order[middle], &record)) {
      return -1;
    }
    FoldNatureName(String(record.name_lat, name, sizeof(name)), folded, sizeof(folded));
    const int compare = std::strcmp(folded, want);
    if (compare == 0) {
      return static_cast<int>(order[middle]);
    }
    if (compare < 0) low = middle + 1;
    else high = middle - 1;
  }
  return -1;
}

}  // namespace lilygo_box::survival
