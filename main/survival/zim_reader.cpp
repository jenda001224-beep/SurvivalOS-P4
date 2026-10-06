/**
 * @Description: Reader for Kiwix ZIM archives
 * @License: GPL 3.0
 */
#include "survival/zim_reader.h"

#include <algorithm>
#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "zstd.h"

namespace lilygo_box::survival {
namespace {

constexpr uint32_t kZimMagic = 0x044D495AU;
constexpr size_t kHeaderBytes = 80;
// A directory entry is a small fixed part plus two zero-terminated strings, so
// one read of this size covers every realistic entry in a single seek.
constexpr size_t kEntryReadBytes = 640;
constexpr uint16_t kRedirectMimetype = 0xFFFFU;
constexpr int kMaxRedirectHops = 8;

// Cluster compression identifiers from the ZIM specification.
constexpr uint8_t kCompressionNone1 = 1;
constexpr uint8_t kCompressionNone2 = 2;
constexpr uint8_t kCompressionZstd = 5;
constexpr uint8_t kExtendedOffsetsFlag = 0x10;

// Clusters in the wild top out well below this; the cap just stops a corrupt
// header from trying to allocate the world.
constexpr size_t kMaxClusterBytes = 24 * 1024 * 1024;
// zstd frames in ZIM are written with the default window, but the header does
// not have to declare a content size, so decompression runs in chunks.
constexpr size_t kDecompressChunkBytes = 256 * 1024;

/**
 * @brief Compares a title against a prefix, treating a match as equal.
 *
 * The comparison is byte-wise and case sensitive because that is exactly how
 * the archive's title index is ordered — verified against a real Czech
 * Wikipedia archive, where `\'Ud` sorts before `\'s-Gravenhage`. Folding case
 * here would break the binary search; SearchTitles() handles what the user
 * typed instead.
 *
 * @param title Title from the archive.
 * @param prefix Text to match.
 * @return Negative when the title sorts before the prefix, zero when the title
 *         starts with it, positive otherwise.
 */
int CompareTitleToPrefix(const char* title, const char* prefix) {
  while (*prefix != '\0') {
    if (*title == '\0') {
      return -1;
    }
    const unsigned char a = static_cast<unsigned char>(*title);
    const unsigned char b = static_cast<unsigned char>(*prefix);
    if (a != b) {
      return a < b ? -1 : 1;
    }
    ++title;
    ++prefix;
  }
  return 0;
}

/**
 * @brief Builds the spellings of a query worth searching for.
 *
 * The index is case sensitive but people type in whatever case they like, and
 * encyclopedia titles are nearly always capitalised, so the query is tried as
 * typed, capitalised and lowercased.
 *
 * @param query Text the user typed.
 * @param variants Output array of at least three buffers.
 * @param variant_size Size of each buffer.
 * @return Number of distinct variants written.
 */
uint32_t BuildQueryVariants(
    const char* query, char variants[][kZimTitleMaxLength],
    size_t variant_size) {
  uint32_t count = 0;
  auto add = [&](const char* candidate) {
    for (uint32_t index = 0; index < count; ++index) {
      if (std::strcmp(variants[index], candidate) == 0) {
        return;
      }
    }
    if (count < 3) {
      std::snprintf(variants[count++], variant_size, "%s", candidate);
    }
  };

  char buffer[kZimTitleMaxLength] = {};
  std::snprintf(buffer, sizeof(buffer), "%s", query);
  add(buffer);

  // Capitalised: only the leading ASCII letter changes, which is all that
  // matters for where the binary search lands.
  if (buffer[0] >= 'a' && buffer[0] <= 'z') {
    char capitalised[kZimTitleMaxLength] = {};
    std::snprintf(capitalised, sizeof(capitalised), "%s", buffer);
    capitalised[0] = static_cast<char>(capitalised[0] - 'a' + 'A');
    add(capitalised);
  } else if (buffer[0] >= 'A' && buffer[0] <= 'Z') {
    char lowered[kZimTitleMaxLength] = {};
    std::snprintf(lowered, sizeof(lowered), "%s", buffer);
    lowered[0] = static_cast<char>(lowered[0] - 'A' + 'a');
    add(lowered);
  }
  return count;
}

/**
 * @brief Copies a zero-terminated string out of a buffer.
 * @param source Buffer to read from.
 * @param available Bytes left in the buffer.
 * @param destination Output buffer.
 * @param capacity Output buffer size.
 * @param consumed Bytes consumed including the terminator.
 * @return true when a terminated string was found.
 */
bool CopyCString(const uint8_t* source, size_t available, char* destination,
    size_t capacity, size_t* consumed) {
  size_t length = 0;
  while (length < available && source[length] != 0) {
    ++length;
  }
  if (length >= available) {
    return false;
  }
  const size_t copy = std::min(length, capacity - 1);
  std::memcpy(destination, source, copy);
  destination[copy] = '\0';
  *consumed = length + 1;
  return true;
}

}  // namespace

ZimArchive::~ZimArchive() { Close(); }

bool ZimArchive::ReadHeader() {
  uint8_t header[kHeaderBytes] = {};
  if (!file_.ReadAt(0, header, sizeof(header))) {
    return false;
  }

  uint32_t magic = 0;
  std::memcpy(&magic, header, sizeof(magic));
  if (magic != kZimMagic) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Not a ZIM archive (magic 0x%08X)\n",
        static_cast<unsigned int>(magic));
    return false;
  }

  uint16_t major = 0;
  std::memcpy(&major, header + 4, sizeof(major));
  std::memcpy(&entry_count_, header + 24, sizeof(entry_count_));
  std::memcpy(&cluster_count_, header + 28, sizeof(cluster_count_));
  std::memcpy(&url_pointer_pos_, header + 32, sizeof(url_pointer_pos_));
  std::memcpy(&cluster_pointer_pos_, header + 48,
      sizeof(cluster_pointer_pos_));
  std::memcpy(&mime_list_pos_, header + 56, sizeof(mime_list_pos_));
  std::memcpy(&main_page_index_, header + 64, sizeof(main_page_index_));
  std::memcpy(&checksum_pos_, header + 72, sizeof(checksum_pos_));

  if (entry_count_ == 0 || cluster_count_ == 0) {
    return false;
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "ZIM v%u: %u entries, %u clusters\n", static_cast<unsigned int>(major),
      static_cast<unsigned int>(entry_count_),
      static_cast<unsigned int>(cluster_count_));
  return true;
}

bool ZimArchive::EntryOffset(uint32_t index, uint64_t* offset) {
  if (index >= entry_count_ || offset == nullptr) {
    return false;
  }
  return file_.ReadUint64(
      url_pointer_pos_ + static_cast<uint64_t>(index) * 8, offset);
}

bool ZimArchive::ReadEntry(uint32_t index, ZimEntry* entry) {
  if (entry == nullptr) {
    return false;
  }
  uint64_t offset = 0;
  if (!EntryOffset(index, &offset)) {
    return false;
  }

  uint8_t raw[kEntryReadBytes] = {};
  size_t available = sizeof(raw);
  if (offset + available > file_.Size()) {
    available = static_cast<size_t>(file_.Size() - offset);
  }
  if (available < 16 || !file_.ReadAt(offset, raw, available)) {
    return false;
  }

  *entry = ZimEntry();
  entry->index = index;
  std::memcpy(&entry->mimetype, raw, sizeof(entry->mimetype));
  entry->name_space = static_cast<char>(raw[3]);

  size_t cursor = 0;
  if (entry->mimetype == kRedirectMimetype) {
    entry->redirect = true;
    std::memcpy(&entry->redirect_index, raw + 8,
        sizeof(entry->redirect_index));
    cursor = 12;
  } else {
    std::memcpy(&entry->cluster, raw + 8, sizeof(entry->cluster));
    std::memcpy(&entry->blob, raw + 12, sizeof(entry->blob));
    cursor = 16;
  }

  size_t consumed = 0;
  if (!CopyCString(raw + cursor, available - cursor, entry->url,
          sizeof(entry->url), &consumed)) {
    return false;
  }
  cursor += consumed;
  if (!CopyCString(raw + cursor, available - cursor, entry->title,
          sizeof(entry->title), &consumed)) {
    return false;
  }
  // ZIM leaves the title empty when it is the same as the url.
  if (entry->title[0] == '\0') {
    std::snprintf(entry->title, sizeof(entry->title), "%s", entry->url);
  }
  return true;
}

bool ZimArchive::FindByUrl(char name_space, const char* url, uint32_t* index) {
  if (url == nullptr || index == nullptr || entry_count_ == 0) {
    return false;
  }

  // Entries are sorted by namespace then url, so a plain binary search finds
  // any entry in about twenty reads even in a million-entry archive.
  uint32_t low = 0;
  uint32_t high = entry_count_ - 1;
  while (low <= high) {
    const uint32_t middle = low + (high - low) / 2;
    ZimEntry entry;
    if (!ReadEntry(middle, &entry)) {
      return false;
    }
    int comparison = 0;
    if (entry.name_space != name_space) {
      comparison = entry.name_space < name_space ? -1 : 1;
    } else {
      comparison = std::strcmp(entry.url, url);
    }
    if (comparison == 0) {
      *index = middle;
      return true;
    }
    if (comparison < 0) {
      low = middle + 1;
    } else {
      if (middle == 0) {
        return false;
      }
      high = middle - 1;
    }
  }
  return false;
}

bool ZimArchive::LoadTitleIndex() {
  uint32_t listing_index = 0;
  // ZIM 6 keeps the title order in an entry rather than in the header. v1 lists
  // every entry, v0 only the articles; either is fine, v1 is preferred.
  if (!FindByUrl('X', "listing/titleOrdered/v1", &listing_index) &&
      !FindByUrl('X', "listing/titleOrdered/v0", &listing_index)) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "ZIM has no title listing entry\n");
    return false;
  }

  ZimEntry listing;
  if (!ReadEntry(listing_index, &listing) || listing.redirect) {
    return false;
  }

  ZimBlob blob;
  if (!ReadBlob(listing.cluster, listing.blob, &blob)) {
    return false;
  }
  const uint32_t count = static_cast<uint32_t>(blob.size / sizeof(uint32_t));
  if (count == 0) {
    FreeBlob(&blob);
    return false;
  }

  auto* index = static_cast<uint32_t*>(heap_caps_malloc(
      count * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (index == nullptr) {
    FreeBlob(&blob);
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Allocate title index (%u entries) failed\n",
        static_cast<unsigned int>(count));
    return false;
  }
  std::memcpy(index, blob.data, count * sizeof(uint32_t));
  FreeBlob(&blob);

  title_index_ = index;
  title_count_ = count;
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "Loaded title index: %u entries\n", static_cast<unsigned int>(count));
  return true;
}

bool ZimArchive::TitleIndexAt(uint32_t position, uint32_t* index) {
  if (index == nullptr || position >= title_count_) {
    return false;
  }
  if (title_index_ != nullptr) {
    *index = title_index_[position];
    return true;
  }
  return false;
}

bool ZimArchive::ReadEntryByTitleOrder(uint32_t position, ZimEntry* entry) {
  uint32_t index = 0;
  return TitleIndexAt(position, &index) && ReadEntry(index, entry);
}

bool ZimArchive::FindTitlePosition(const char* prefix, uint32_t* position) {
  if (prefix == nullptr || position == nullptr || title_count_ == 0) {
    return false;
  }

  uint32_t low = 0;
  uint32_t high = title_count_;
  while (low < high) {
    const uint32_t middle = low + (high - low) / 2;
    ZimEntry entry;
    if (!ReadEntryByTitleOrder(middle, &entry)) {
      return false;
    }
    if (CompareTitleToPrefix(entry.title, prefix) < 0) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  *position = low;
  return true;
}

uint32_t ZimArchive::SearchTitles(
    const char* query, uint32_t* results, uint32_t capacity) {
  if (query == nullptr || results == nullptr || capacity == 0 ||
      title_count_ == 0) {
    return 0;
  }

  // An empty query just lists the archive from the top.
  if (query[0] == '\0') {
    uint32_t found = 0;
    for (uint32_t position = 0;
        position < title_count_ && found < capacity; ++position) {
      ZimEntry entry;
      if (!ReadEntryByTitleOrder(position, &entry)) {
        break;
      }
      if (entry.name_space != 'C' && entry.name_space != 'A') {
        continue;
      }
      results[found++] = entry.index;
    }
    return found;
  }

  char variants[3][kZimTitleMaxLength] = {};
  const uint32_t variant_count =
      BuildQueryVariants(query, variants, kZimTitleMaxLength);

  uint32_t found = 0;
  for (uint32_t variant = 0; variant < variant_count && found < capacity;
      ++variant) {
    uint32_t position = 0;
    if (!FindTitlePosition(variants[variant], &position)) {
      continue;
    }
    const size_t prefix_length = std::strlen(variants[variant]);
    for (uint32_t offset = 0;
        position + offset < title_count_ && found < capacity; ++offset) {
      ZimEntry entry;
      if (!ReadEntryByTitleOrder(position + offset, &entry)) {
        break;
      }
      // The index is sorted, so the first non-matching title ends this run.
      if (std::strncmp(entry.title, variants[variant], prefix_length) != 0) {
        break;
      }
      if (entry.name_space != 'C' && entry.name_space != 'A') {
        continue;
      }
      bool duplicate = false;
      for (uint32_t index = 0; index < found; ++index) {
        if (results[index] == entry.index) {
          duplicate = true;
          break;
        }
      }
      if (!duplicate) {
        results[found++] = entry.index;
      }
    }
  }
  return found;
}

bool ZimArchive::EnsureClusterCached(uint32_t cluster) {
  if (cluster_cache_index_ == cluster && cluster_cache_ != nullptr) {
    return true;
  }
  if (cluster >= cluster_count_) {
    return false;
  }

  uint64_t start = 0;
  if (!file_.ReadUint64(
          cluster_pointer_pos_ + static_cast<uint64_t>(cluster) * 8, &start)) {
    return false;
  }
  // The pointer list holds exactly clusterCount entries, so the last cluster has
  // no successor to bound it; everything after it in the file is index data and
  // the decompressor stops at the frame end anyway.
  uint64_t end = checksum_pos_ > start ? checksum_pos_ : file_.Size();
  if (cluster + 1 < cluster_count_) {
    uint64_t next = 0;
    if (file_.ReadUint64(
            cluster_pointer_pos_ + static_cast<uint64_t>(cluster + 1) * 8,
            &next) &&
        next > start) {
      end = next;
    }
  }
  if (end <= start + 1) {
    return false;
  }

  uint8_t info = 0;
  if (!file_.ReadAt(start, &info, 1)) {
    return false;
  }
  const uint8_t compression = info & 0x0FU;
  const bool wide_offsets = (info & kExtendedOffsetsFlag) != 0;
  const uint64_t payload_offset = start + 1;
  const size_t payload_size = static_cast<size_t>(
      std::min<uint64_t>(end - payload_offset, kMaxClusterBytes));

  size_t plain_size = 0;
  uint8_t* plain = nullptr;

  if (compression == kCompressionNone1 || compression == kCompressionNone2) {
    plain = static_cast<uint8_t*>(heap_caps_malloc(
        payload_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (plain == nullptr ||
        !file_.ReadAt(payload_offset, plain, payload_size)) {
      heap_caps_free(plain);
      return false;
    }
    plain_size = payload_size;
  } else if (compression == kCompressionZstd) {
    auto* compressed = static_cast<uint8_t*>(heap_caps_malloc(
        payload_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (compressed == nullptr) {
      return false;
    }
    if (!file_.ReadAt(payload_offset, compressed, payload_size)) {
      heap_caps_free(compressed);
      return false;
    }

    ZSTD_DStream* stream = ZSTD_createDStream();
    if (stream == nullptr) {
      heap_caps_free(compressed);
      return false;
    }
    ZSTD_initDStream(stream);

    size_t capacity = kDecompressChunkBytes;
    plain = static_cast<uint8_t*>(
        heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (plain == nullptr) {
      ZSTD_freeDStream(stream);
      heap_caps_free(compressed);
      return false;
    }

    ZSTD_inBuffer input = {compressed, payload_size, 0};
    bool failed = false;
    while (true) {
      if (plain_size == capacity) {
        if (capacity >= kMaxClusterBytes) {
          failed = true;
          break;
        }
        const size_t grown = std::min(capacity * 2, kMaxClusterBytes);
        auto* larger = static_cast<uint8_t*>(heap_caps_realloc(
            plain, grown, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (larger == nullptr) {
          failed = true;
          break;
        }
        plain = larger;
        capacity = grown;
      }
      ZSTD_outBuffer output = {plain, capacity, plain_size};
      const size_t result = ZSTD_decompressStream(stream, &output, &input);
      if (ZSTD_isError(result) != 0) {
        LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
            "zstd cluster %u failed: %s\n",
            static_cast<unsigned int>(cluster), ZSTD_getErrorName(result));
        failed = true;
        break;
      }
      plain_size = output.pos;
      // result == 0 means the frame ended; otherwise more output is expected.
      if (result == 0 || (input.pos == input.size && output.pos < capacity)) {
        break;
      }
    }
    ZSTD_freeDStream(stream);
    heap_caps_free(compressed);
    if (failed || plain_size == 0) {
      heap_caps_free(plain);
      return false;
    }
  } else {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Unsupported ZIM cluster compression %u; rebuild the archive with "
        "zstd\n",
        static_cast<unsigned int>(compression));
    return false;
  }

  heap_caps_free(cluster_cache_);
  cluster_cache_ = plain;
  cluster_cache_size_ = plain_size;
  cluster_cache_capacity_ = plain_size;
  cluster_cache_index_ = cluster;
  cluster_cache_wide_offsets_ = wide_offsets;
  return true;
}

bool ZimArchive::ReadBlob(uint32_t cluster, uint32_t blob, ZimBlob* out) {
  if (out == nullptr || !EnsureClusterCached(cluster)) {
    return false;
  }

  const size_t step = cluster_cache_wide_offsets_ ? 8 : 4;
  if (cluster_cache_size_ < step * 2) {
    return false;
  }

  auto read_offset = [this, step](uint32_t slot, uint64_t* value) {
    const size_t position = static_cast<size_t>(slot) * step;
    if (position + step > cluster_cache_size_) {
      return false;
    }
    uint64_t result = 0;
    for (size_t index = step; index-- > 0;) {
      result = (result << 8) | cluster_cache_[position + index];
    }
    *value = result;
    return true;
  };

  uint64_t first_offset = 0;
  if (!read_offset(0, &first_offset) || first_offset < step) {
    return false;
  }
  // The offset table starts the cluster and includes a terminator, so its own
  // size tells you how many blobs the cluster holds.
  const uint32_t blob_count =
      static_cast<uint32_t>(first_offset / step) - 1;
  if (blob >= blob_count) {
    return false;
  }

  uint64_t start = 0;
  uint64_t end = 0;
  if (!read_offset(blob, &start) || !read_offset(blob + 1, &end) ||
      end < start || end > cluster_cache_size_) {
    return false;
  }

  const size_t size = static_cast<size_t>(end - start);
  auto* data = static_cast<uint8_t*>(
      heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (data == nullptr) {
    return false;
  }
  std::memcpy(data, cluster_cache_ + start, size);
  // Article payloads are HTML, so a terminator makes them safe to treat as text.
  data[size] = 0;
  out->data = data;
  out->size = size;
  return true;
}

bool ZimArchive::ResolveEntry(ZimEntry* entry) {
  if (entry == nullptr) {
    return false;
  }
  for (int hop = 0; hop < kMaxRedirectHops; ++hop) {
    if (!entry->redirect) {
      return true;
    }
    if (!ReadEntry(entry->redirect_index, entry)) {
      return false;
    }
  }
  LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
      "Redirect chain too long at entry %u\n",
      static_cast<unsigned int>(entry->index));
  return false;
}

bool ZimArchive::ReadEntryContent(const ZimEntry& entry, ZimBlob* out) {
  ZimEntry resolved = entry;
  if (!ResolveEntry(&resolved)) {
    return false;
  }
  return ReadBlob(resolved.cluster, resolved.blob, out);
}

void ZimArchive::FreeBlob(ZimBlob* blob) {
  if (blob == nullptr || blob->data == nullptr) {
    return;
  }
  heap_caps_free(blob->data);
  blob->data = nullptr;
  blob->size = 0;
}

bool ZimArchive::Open(const char* path) {
  Close();
  if (!file_.Open(path)) {
    return false;
  }
  if (!ReadHeader()) {
    Close();
    return false;
  }
  if (!LoadTitleIndex()) {
    // Browsing by title is gone, but url lookup and article reading still work.
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Continuing without a title index\n");
  }
  return true;
}

void ZimArchive::Close() {
  heap_caps_free(title_index_);
  title_index_ = nullptr;
  title_count_ = 0;
  heap_caps_free(cluster_cache_);
  cluster_cache_ = nullptr;
  cluster_cache_size_ = 0;
  cluster_cache_capacity_ = 0;
  cluster_cache_index_ = 0xFFFFFFFFU;
  entry_count_ = 0;
  cluster_count_ = 0;
  file_.Close();
}

}  // namespace lilygo_box::survival
