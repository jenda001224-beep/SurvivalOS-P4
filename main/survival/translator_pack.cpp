/*
 * @Description: Reader for the Translator's dictionary bundles (.svtr)
 * @License: GPL 3.0
 */
#include "survival/translator_pack.h"

#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"

namespace lilygo_box::survival {
namespace {

constexpr char kMagic[8] = {'S', 'V', 'T', 'R', '1', 0, 0, 0};
constexpr size_t kHeaderBytes = 128;
constexpr size_t kLanguageBytes = 128;
// How many blocks past the first one a search may walk.
constexpr int kMaxScanBlocks = 4;

constexpr const char* kPartsOfSpeech[] = {"", "noun", "verb", "adj", "adv",
    "name", "pron", "num", "phrase", "prep", "conj", "intj", "det", "particle",
    "prefix", "suffix", "abbrev", "article", "postp", "proverb", "character",
    "symbol", "contraction", "affix", "other"};

uint16_t Le16(const uint8_t* data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}
uint32_t Le32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
      (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}
uint64_t Le64(const uint8_t* data) {
  return static_cast<uint64_t>(Le32(data)) |
      (static_cast<uint64_t>(Le32(data + 4)) << 32);
}

void* PsramAlloc(size_t bytes) {
  return heap_caps_malloc(bytes == 0 ? 1 : bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// Copies a length-prefixed field into a NUL-terminated buffer.
bool TakeField(const uint8_t* data, size_t size, size_t* cursor, size_t length,
    char* out, size_t out_size) {
  if (*cursor + length > size) {
    return false;
  }
  const size_t copy = std::min(length, out_size - 1);
  std::memcpy(out, data + *cursor, copy);
  out[copy] = '\0';
  *cursor += length;
  return true;
}

int CompareKey(const char* stored, const char* key) {
  // Stored block keys are cut at 32 bytes and zero padded.
  return std::strncmp(stored, key, kTranslatorBlockKeyBytes);
}

bool MakeDirectories(const char* path) {
  char buffer[200] = {};
  std::snprintf(buffer, sizeof(buffer), "%s", path);
  for (char* slash = std::strchr(buffer + 1, '/'); slash != nullptr;
       slash = std::strchr(slash + 1, '/')) {
    *slash = '\0';
    mkdir(buffer, 0775);
    *slash = '/';
  }
  return mkdir(buffer, 0775) == 0 || errno == EEXIST;
}

}  // namespace

const char* TranslatorPartOfSpeechName(uint8_t code) {
  return code < sizeof(kPartsOfSpeech) / sizeof(kPartsOfSpeech[0])
      ? kPartsOfSpeech[code]
      : "";
}

TranslatorPack::~TranslatorPack() { Close(); }

void TranslatorPack::Close() {
  file_.Close();
  heap_caps_free(languages_);
  heap_caps_free(phrases_);
  heap_caps_free(topic_text_);
  heap_caps_free(topic_offsets_);
  heap_caps_free(fold_codes_);
  heap_caps_free(fold_offsets_);
  heap_caps_free(fold_pool_);
  heap_caps_free(block_);
  for (KeyCache& cache : key_cache_) {
    heap_caps_free(cache.keys);
    cache = KeyCache();
  }
  languages_ = nullptr;
  phrases_ = nullptr;
  topic_text_ = nullptr;
  topic_offsets_ = nullptr;
  fold_codes_ = nullptr;
  fold_offsets_ = nullptr;
  fold_pool_ = nullptr;
  block_ = nullptr;
  language_count_ = concept_count_ = phrase_count_ = fold_count_ = 0;
  topic_count_ = 0;
  block_language_ = -1;
  block_index_ = UINT32_MAX;
}

bool TranslatorPack::Open(const char* path) {
  Close();
  if (!file_.Open(path)) {
    return false;
  }
  uint8_t header[kHeaderBytes] = {};
  if (!file_.ReadAt(0, header, sizeof(header)) ||
      std::memcmp(header, kMagic, sizeof(kMagic)) != 0 || Le32(header + 8) != 1) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Not a translator bundle: %s\n", path);
    Close();
    return false;
  }
  language_count_ = Le32(header + 12);
  concept_count_ = Le32(header + 16);
  phrase_count_ = Le32(header + 20);
  const uint64_t languages_offset = Le64(header + 24);
  concepts_offset_ = Le64(header + 32);
  concept_text_offset_ = Le64(header + 40);
  const uint64_t phrases_offset = Le64(header + 48);
  const uint64_t topics_offset = Le64(header + 56);
  speech_offset_ = Le64(header + 64);
  speech_bytes_ = Le64(header + 72);
  const uint64_t fold_offset = Le64(header + 80);
  const uint64_t fold_bytes = Le64(header + 88);

  languages_ = static_cast<TranslatorLanguage*>(
      PsramAlloc(sizeof(TranslatorLanguage) * language_count_));
  auto* raw = static_cast<uint8_t*>(PsramAlloc(kLanguageBytes * language_count_));
  block_ = static_cast<uint8_t*>(PsramAlloc(kTranslatorBlockBytes));
  if (languages_ == nullptr || raw == nullptr || block_ == nullptr ||
      !file_.ReadAt(languages_offset, raw, kLanguageBytes * language_count_)) {
    heap_caps_free(raw);
    Close();
    return false;
  }
  for (uint32_t i = 0; i < language_count_; ++i) {
    const uint8_t* entry = raw + i * kLanguageBytes;
    TranslatorLanguage& language = languages_[i];
    language = TranslatorLanguage();
    std::memcpy(language.code, entry, 11);
    std::memcpy(language.name, entry + 12, 35);
    std::memcpy(language.voice, entry + 48, 15);
    language.entry_count = Le32(entry + 64);
    language.block_count = Le32(entry + 68);
    language.blocks_offset = Le64(entry + 72);
    language.keys_offset = Le64(entry + 80);
    language.map_offset = Le64(entry + 88);
    language.map_count = Le32(entry + 96);
  }
  heap_caps_free(raw);

  // Phrases: u32 concept, u16 topic, u16 padding.
  phrases_ = static_cast<TranslatorPhrase*>(
      PsramAlloc(sizeof(TranslatorPhrase) * phrase_count_));
  auto* phrase_raw = static_cast<uint8_t*>(PsramAlloc(8 * phrase_count_));
  if (phrases_ == nullptr || phrase_raw == nullptr ||
      !file_.ReadAt(phrases_offset, phrase_raw, 8 * phrase_count_)) {
    heap_caps_free(phrase_raw);
    Close();
    return false;
  }
  for (uint32_t i = 0; i < phrase_count_; ++i) {
    phrases_[i].concept_id = Le32(phrase_raw + i * 8);
    phrases_[i].topic = Le16(phrase_raw + i * 8 + 4);
  }
  heap_caps_free(phrase_raw);

  // Topics: u16 count, then u8 length + text each.
  uint16_t topic_count = 0;
  if (file_.ReadUint16(topics_offset, &topic_count) && topic_count > 0) {
    const size_t bytes = static_cast<size_t>(topic_count) * 62;
    auto* text = static_cast<uint8_t*>(PsramAlloc(bytes));
    topic_text_ = static_cast<char*>(PsramAlloc(bytes));
    topic_offsets_ = static_cast<uint16_t*>(PsramAlloc(topic_count * sizeof(uint16_t)));
    if (text != nullptr && topic_text_ != nullptr && topic_offsets_ != nullptr) {
      const uint64_t available = speech_offset_ - topics_offset - 2;
      const size_t read = static_cast<size_t>(std::min<uint64_t>(bytes, available));
      if (file_.ReadAt(topics_offset + 2, text, read)) {
        size_t in = 0, out = 0;
        for (int i = 0; i < topic_count && in < read; ++i) {
          const size_t length = text[in++];
          if (in + length > read) {
            break;
          }
          topic_offsets_[i] = static_cast<uint16_t>(out);
          std::memcpy(topic_text_ + out, text + in, length);
          topic_text_[out + length] = '\0';
          out += length + 1;
          in += length;
          topic_count_ = i + 1;
        }
      }
    }
    heap_caps_free(text);
  }

  // Fold table: u32 count, (u32 code, u32 offset) entries, then the pool.
  if (fold_bytes > 4) {
    uint32_t count = 0;
    if (file_.ReadUint32(fold_offset, &count) && count > 0 &&
        4 + static_cast<uint64_t>(count) * 8 < fold_bytes) {
      const size_t pool_bytes = static_cast<size_t>(fold_bytes - 4 - count * 8ULL);
      auto* table = static_cast<uint8_t*>(PsramAlloc(count * 8));
      fold_codes_ = static_cast<uint32_t*>(PsramAlloc(count * 4));
      fold_offsets_ = static_cast<uint32_t*>(PsramAlloc(count * 4));
      fold_pool_ = static_cast<char*>(PsramAlloc(pool_bytes));
      if (table != nullptr && fold_codes_ != nullptr && fold_offsets_ != nullptr &&
          fold_pool_ != nullptr && file_.ReadAt(fold_offset + 4, table, count * 8) &&
          file_.ReadAt(fold_offset + 4 + count * 8ULL, fold_pool_, pool_bytes)) {
        for (uint32_t i = 0; i < count; ++i) {
          fold_codes_[i] = Le32(table + i * 8);
          fold_offsets_[i] = Le32(table + i * 8 + 4);
        }
        fold_count_ = count;
      }
      heap_caps_free(table);
    }
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "Translator bundle: %u languages, %u concepts, %u phrases\n",
      static_cast<unsigned>(language_count_), static_cast<unsigned>(concept_count_),
      static_cast<unsigned>(phrase_count_));
  return true;
}

int TranslatorPack::FindLanguage(const char* code) const {
  for (uint32_t i = 0; i < language_count_; ++i) {
    if (std::strcmp(languages_[i].code, code) == 0) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

const char* TranslatorPack::Topic(int index) const {
  if (index < 0 || index >= topic_count_ || topic_text_ == nullptr) {
    return "";
  }
  return topic_text_ + topic_offsets_[index];
}

uint32_t TranslatorPack::FoldCodepoint(uint32_t code, const char** replacement) const {
  *replacement = nullptr;
  if (code < 0x80) {
    return (code >= 'A' && code <= 'Z') ? code + 32 : code;
  }
  const uint32_t* begin = fold_codes_;
  const uint32_t* end = fold_codes_ + fold_count_;
  const uint32_t* found = std::lower_bound(begin, end, code);
  if (found != end && *found == code) {
    *replacement = fold_pool_ + fold_offsets_[found - fold_codes_];
  }
  return code;
}

size_t TranslatorPack::Fold(const char* text, char* out, size_t size) const {
  if (out == nullptr || size == 0) {
    return 0;
  }
  size_t length = 0;
  const auto* bytes = reinterpret_cast<const unsigned char*>(text != nullptr ? text : "");
  auto append = [&](const char* data, size_t count) {
    if (length + count + 1 > size) {
      return false;
    }
    std::memcpy(out + length, data, count);
    length += count;
    return true;
  };
  while (*bytes != 0) {
    const unsigned char* start = bytes;
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
    const char* replacement = nullptr;
    const uint32_t folded = FoldCodepoint(code, &replacement);
    bool ok = true;
    if (replacement != nullptr) {
      ok = append(replacement, std::strlen(replacement));
    } else if (code < 0x80) {
      const char letter = static_cast<char>(folded);
      ok = append(&letter, 1);
    } else {
      ok = append(reinterpret_cast<const char*>(start),
          static_cast<size_t>(bytes - start));
    }
    if (!ok) {
      break;
    }
  }
  // Trim spaces at both ends, as the packer's fold() does.
  size_t begin = 0;
  while (begin < length && out[begin] == ' ') {
    ++begin;
  }
  while (length > begin && out[length - 1] == ' ') {
    --length;
  }
  if (begin > 0) {
    std::memmove(out, out + begin, length - begin);
    length -= begin;
  }
  out[length] = '\0';
  return length;
}

bool TranslatorPack::LoadKeys(int language) {
  for (const KeyCache& cache : key_cache_) {
    if (cache.language == language && cache.keys != nullptr) {
      return true;
    }
  }
  KeyCache& slot = key_cache_[next_key_slot_];
  next_key_slot_ = (next_key_slot_ + 1) % 2;
  heap_caps_free(slot.keys);
  slot = KeyCache();
  const TranslatorLanguage& info = languages_[language];
  const size_t bytes = static_cast<size_t>(info.block_count) * kTranslatorBlockKeyBytes;
  slot.keys = static_cast<char*>(PsramAlloc(bytes));
  if (slot.keys == nullptr || !file_.ReadAt(info.keys_offset, slot.keys, bytes)) {
    heap_caps_free(slot.keys);
    slot.keys = nullptr;
    return false;
  }
  slot.language = language;
  return true;
}

int TranslatorPack::LowerBlock(int language, const char* key) const {
  const char* keys = nullptr;
  for (const KeyCache& cache : key_cache_) {
    if (cache.language == language) {
      keys = cache.keys;
    }
  }
  if (keys == nullptr) {
    return 0;
  }
  // Last block whose first key sorts before the key; equal keys may continue
  // from the block before, so "before" is strict.
  int low = 0;
  int high = static_cast<int>(languages_[language].block_count) - 1;
  int found = 0;
  while (low <= high) {
    const int middle = (low + high) / 2;
    if (CompareKey(keys + static_cast<size_t>(middle) * kTranslatorBlockKeyBytes, key) < 0) {
      found = middle;
      low = middle + 1;
    } else {
      high = middle - 1;
    }
  }
  return found;
}

bool TranslatorPack::ReadBlock(int language, uint32_t block) {
  if (block_language_ == language && block_index_ == block) {
    return true;
  }
  const TranslatorLanguage& info = languages_[language];
  if (block >= info.block_count ||
      !file_.ReadAt(info.blocks_offset + static_cast<uint64_t>(block) * kTranslatorBlockBytes,
          block_, kTranslatorBlockBytes)) {
    block_language_ = -1;
    return false;
  }
  block_language_ = language;
  block_index_ = block;
  return true;
}

bool TranslatorPack::DecodeRecord(const uint8_t* data, size_t size, size_t* used,
    TranslatorRecord* out) const {
  size_t cursor = 0;
  auto byte = [&](uint8_t* value) {
    if (cursor >= size) return false;
    *value = data[cursor++];
    return true;
  };
  uint8_t length = 0;
  if (!byte(&length) || !TakeField(data, size, &cursor, length, out->key, sizeof(out->key)) ||
      !byte(&length) || !TakeField(data, size, &cursor, length, out->word, sizeof(out->word)) ||
      !byte(&length) || !TakeField(data, size, &cursor, length, out->roman, sizeof(out->roman))) {
    return false;
  }
  if (cursor + 2 > size) {
    return false;
  }
  const uint16_t gloss_length = Le16(data + cursor);
  cursor += 2;
  if (!TakeField(data, size, &cursor, gloss_length, out->glosses, sizeof(out->glosses)) ||
      !byte(&length) || !TakeField(data, size, &cursor, length, out->lemma, sizeof(out->lemma)) ||
      !byte(&out->part_of_speech) || !byte(&length)) {
    return false;
  }
  if (cursor + length * 4U > size) {
    return false;
  }
  out->concept_count = std::min<uint8_t>(length, kTranslatorMaxConcepts);
  for (int i = 0; i < out->concept_count; ++i) {
    out->concepts[i] = Le32(data + cursor + i * 4);
  }
  cursor += length * 4U;
  *used = cursor;
  return true;
}

bool TranslatorPack::ReadRecord(int language, uint32_t offset, TranslatorRecord* out) {
  if (language < 0 || language >= static_cast<int>(language_count_) || out == nullptr) {
    return false;
  }
  const uint32_t block = offset / kTranslatorBlockBytes;
  const size_t inside = offset % kTranslatorBlockBytes;
  if (inside < 2 || !ReadBlock(language, block)) {
    return false;
  }
  size_t used = 0;
  if (!DecodeRecord(block_ + inside, kTranslatorBlockBytes - inside, &used, out)) {
    return false;
  }
  out->offset = offset;
  return true;
}

int TranslatorPack::Find(int language, const char* query, bool prefix,
    TranslatorRecord* out, int capacity) {
  if (language < 0 || language >= static_cast<int>(language_count_) ||
      out == nullptr || capacity <= 0 || !LoadKeys(language)) {
    return 0;
  }
  char key[96] = {};
  const size_t key_length = Fold(query, key, sizeof(key));
  if (key_length == 0) {
    return 0;
  }
  const TranslatorLanguage& info = languages_[language];
  int count = 0;
  bool started = false;
  int scanned_after_start = 0;
  for (uint32_t block = static_cast<uint32_t>(LowerBlock(language, key));
       block < info.block_count && count < capacity; ++block) {
    if (!ReadBlock(language, block)) {
      break;
    }
    const uint16_t records = Le16(block_);
    size_t cursor = 2;
    for (uint16_t i = 0; i < records && count < capacity; ++i) {
      TranslatorRecord& record = out[count];
      size_t used = 0;
      if (!DecodeRecord(block_ + cursor, kTranslatorBlockBytes - cursor, &used, &record)) {
        return count;
      }
      record.offset = block * kTranslatorBlockBytes + static_cast<uint32_t>(cursor);
      cursor += used;
      const int order = std::strcmp(record.key, key);
      const bool match = prefix
          ? std::strncmp(record.key, key, key_length) == 0
          : order == 0;
      if (match) {
        started = true;
        ++count;
      } else if (order > 0) {
        return count;
      }
    }
    if (started && ++scanned_after_start > kMaxScanBlocks) {
      break;
    }
    if (!started && block > static_cast<uint32_t>(LowerBlock(language, key)) + 1) {
      break;
    }
  }
  return count;
}

int TranslatorPack::ConceptWords(int language, uint32_t concept_id, uint32_t* offsets,
    int capacity) {
  if (language < 0 || language >= static_cast<int>(language_count_) ||
      offsets == nullptr || capacity <= 0) {
    return 0;
  }
  const TranslatorLanguage& info = languages_[language];
  // First map entry with this concept (entries are u32 concept, u32 offset).
  uint32_t low = 0;
  uint32_t high = info.map_count;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    uint32_t value = 0;
    if (!file_.ReadUint32(info.map_offset + static_cast<uint64_t>(middle) * 8, &value)) {
      return 0;
    }
    if (value < concept_id) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  int count = 0;
  uint8_t entry[8 * 8] = {};
  while (count < capacity && low < info.map_count) {
    const uint32_t batch = std::min<uint32_t>(8, info.map_count - low);
    if (!file_.ReadAt(info.map_offset + static_cast<uint64_t>(low) * 8, entry, batch * 8)) {
      break;
    }
    for (uint32_t i = 0; i < batch && count < capacity; ++i) {
      if (Le32(entry + i * 8) != concept_id) {
        return count;
      }
      offsets[count++] = Le32(entry + i * 8 + 4);
    }
    low += batch;
  }
  return count;
}

bool TranslatorPack::Concept(uint32_t concept_id, char* word, size_t word_size,
    char* sense, size_t sense_size) {
  if (concept_id >= concept_count_) {
    return false;
  }
  uint32_t offset = 0;
  if (!file_.ReadUint32(concepts_offset_ + static_cast<uint64_t>(concept_id) * 4, &offset)) {
    return false;
  }
  char text[400] = {};
  if (!file_.ReadAt(concept_text_offset_ + offset, text, sizeof(text) - 1)) {
    // Near the end of the area a full read can run past it; try shorter.
    if (!file_.ReadAt(concept_text_offset_ + offset, text, 64)) {
      return false;
    }
  }
  text[sizeof(text) - 1] = '\0';
  char* separator = std::strchr(text, '\x1f');
  if (separator != nullptr) {
    *separator = '\0';
  }
  if (word != nullptr && word_size > 0) {
    std::snprintf(word, word_size, "%s", text);
  }
  if (sense != nullptr && sense_size > 0) {
    std::snprintf(sense, sense_size, "%s", separator != nullptr ? separator + 1 : "");
  }
  return true;
}

bool TranslatorPack::ExtractResources(const char* directory) {
  if (directory == nullptr || speech_bytes_ == 0) {
    return false;
  }
  char marker_path[200] = {};
  std::snprintf(marker_path, sizeof(marker_path), "%s/.svtr-resources", directory);
  char marker[64] = {};
  std::snprintf(marker, sizeof(marker), "%llu:%llu:%llu",
      static_cast<unsigned long long>(file_.Size()),
      static_cast<unsigned long long>(speech_offset_),
      static_cast<unsigned long long>(speech_bytes_));
  if (FILE* existing = std::fopen(marker_path, "r")) {
    char seen[64] = {};
    const bool same = std::fgets(seen, sizeof(seen), existing) != nullptr &&
        std::strcmp(seen, marker) == 0;
    std::fclose(existing);
    if (same) {
      return true;
    }
  }
  if (!MakeDirectories(directory)) {
    return false;
  }
  constexpr size_t kChunk = 16 * 1024;
  auto* buffer = static_cast<uint8_t*>(PsramAlloc(kChunk));
  if (buffer == nullptr) {
    return false;
  }
  bool ok = true;
  uint64_t cursor = speech_offset_;
  const uint64_t end = speech_offset_ + speech_bytes_;
  uint8_t header[512] = {};
  while (ok && cursor + 512 <= end) {
    if (!file_.ReadAt(cursor, header, sizeof(header))) {
      ok = false;
      break;
    }
    cursor += 512;
    if (header[0] == '\0') {
      break;  // end of archive
    }
    char name[256] = {};
    char prefix[156] = {};
    std::memcpy(prefix, header + 345, 155);
    std::memcpy(name, header + 0, 100);
    char size_text[13] = {};
    std::memcpy(size_text, header + 124, 12);
    const uint64_t size = std::strtoull(size_text, nullptr, 8);
    const char type = static_cast<char>(header[156]);
    char path[320] = {};
    if (prefix[0] != '\0') {
      std::snprintf(path, sizeof(path), "%s/%s/%s", directory, prefix, name);
    } else {
      std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    }
    if (std::strstr(name, "..") != nullptr) {
      ok = false;
      break;
    }
    if (type == '5') {
      MakeDirectories(path);
    } else if (type == '0' || type == '\0') {
      char* slash = std::strrchr(path, '/');
      if (slash != nullptr) {
        *slash = '\0';
        MakeDirectories(path);
        *slash = '/';
      }
      FILE* out = std::fopen(path, "wb");
      if (out == nullptr) {
        ok = false;
        break;
      }
      uint64_t left = size;
      uint64_t from = cursor;
      while (left > 0) {
        const size_t take = static_cast<size_t>(std::min<uint64_t>(left, kChunk));
        if (!file_.ReadAt(from, buffer, take) || std::fwrite(buffer, 1, take, out) != take) {
          ok = false;
          break;
        }
        from += take;
        left -= take;
      }
      std::fclose(out);
    }
    cursor += (size + 511) / 512 * 512;
  }
  heap_caps_free(buffer);
  if (ok) {
    if (FILE* out = std::fopen(marker_path, "w")) {
      std::fputs(marker, out);
      std::fclose(out);
    }
  }
  return ok;
}

}  // namespace lilygo_box::survival
