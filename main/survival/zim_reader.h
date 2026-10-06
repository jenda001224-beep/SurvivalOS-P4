/**
 * @Description: Reader for Kiwix ZIM archives
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "survival/split_file.h"

namespace lilygo_box::survival {

inline constexpr size_t kZimUrlMaxLength = 256;
inline constexpr size_t kZimTitleMaxLength = 256;

// One directory entry out of the archive.
struct ZimEntry {
  uint32_t index = 0;
  // Redirects point at another entry instead of carrying content themselves.
  bool redirect = false;
  uint32_t redirect_index = 0;
  uint32_t cluster = 0;
  uint32_t blob = 0;
  uint16_t mimetype = 0;
  char name_space = 0;
  char url[kZimUrlMaxLength] = {};
  // ZIM stores an empty title when it matches the url; this is always filled in.
  char title[kZimTitleMaxLength] = {};
};

// A decompressed payload. Release it with ZimArchive::FreeBlob().
struct ZimBlob {
  uint8_t* data = nullptr;
  size_t size = 0;
};

// Reads Kiwix ZIM archives, including ones split across FAT32-sized parts.
//
// Only what an offline reader needs is implemented: entry lookup by url, title
// ordered browsing and prefix search, redirect resolution and blob extraction.
// The Xapian full-text index that ZIM can carry is deliberately ignored — it is
// far too heavy for this hardware, and title search covers the practical cases.
class ZimArchive {
 public:
  ZimArchive() = default;
  ~ZimArchive();

  ZimArchive(const ZimArchive&) = delete;
  ZimArchive& operator=(const ZimArchive&) = delete;

  /**
   * @brief Opens an archive and loads its header and title index.
   * @param path Path to the .zim file, or to the base name of its parts.
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
   * @brief Number of directory entries in the archive.
   * @return Entry count.
   */
  uint32_t entry_count() const { return entry_count_; }

  /**
   * @brief Number of entries reachable through the title index.
   * @return Title index length.
   */
  uint32_t title_count() const { return title_count_; }

  /**
   * @brief Index of the entry the archive nominates as its front page.
   * @return Entry index, or 0xFFFFFFFF when the archive has none.
   */
  uint32_t main_page_index() const { return main_page_index_; }

  /**
   * @brief Reads one directory entry.
   * @param index Entry index.
   * @param entry Entry output.
   * @return true when the entry was read.
   */
  bool ReadEntry(uint32_t index, ZimEntry* entry);

  /**
   * @brief Reads the entry at a position in title order.
   * @param position Position in the title index.
   * @param entry Entry output.
   * @return true when the entry was read.
   */
  bool ReadEntryByTitleOrder(uint32_t position, ZimEntry* entry);

  /**
   * @brief Finds the first title-ordered position at or after a prefix.
   * @param prefix Search text; matched byte-wise and case sensitively,
   *        because that is how the archive orders its titles.
   * @param position Output position, clamped to the end of the index.
   * @return true when the search ran.
   */
  bool FindTitlePosition(const char* prefix, uint32_t* position);

  /**
   * @brief Searches article titles for a prefix.
   *
   * The archive's title index is byte-ordered and case sensitive, so the query
   * is tried in a few spellings and the runs are merged. Only content entries
   * are returned.
   *
   * @param query Text the user typed; empty lists the archive from the start.
   * @param results Output array of entry indices.
   * @param capacity Size of the output array.
   * @return Number of results written.
   */
  uint32_t SearchTitles(
      const char* query, uint32_t* results, uint32_t capacity);

  /**
   * @brief Looks an entry up by namespace and url.
   * @param name_space ZIM namespace, 'C' for content in ZIM 6.
   * @param url Entry url.
   * @param index Output entry index.
   * @return true when an exact match was found.
   */
  bool FindByUrl(char name_space, const char* url, uint32_t* index);

  /**
   * @brief Follows redirects until a real entry is reached.
   * @param entry Entry to resolve in place.
   * @return true when a non-redirect entry was reached.
   */
  bool ResolveEntry(ZimEntry* entry);

  /**
   * @brief Reads and decompresses one blob out of a cluster.
   * @param cluster Cluster index.
   * @param blob Blob index inside the cluster.
   * @param out Blob output; the caller frees it with FreeBlob().
   * @return true when the blob was read.
   */
  bool ReadBlob(uint32_t cluster, uint32_t blob, ZimBlob* out);

  /**
   * @brief Reads the content of an entry, following any redirect first.
   * @param entry Entry to read.
   * @param out Blob output; the caller frees it with FreeBlob().
   * @return true when content was read.
   */
  bool ReadEntryContent(const ZimEntry& entry, ZimBlob* out);

  /**
   * @brief Releases a blob returned by ReadBlob() or ReadEntryContent().
   * @param blob Blob to free.
   */
  void FreeBlob(ZimBlob* blob);

 private:
  /**
   * @brief Reads the fixed header at the start of the archive.
   * @return true when the header is a supported ZIM archive.
   */
  bool ReadHeader();

  /**
   * @brief Loads the title-ordered index into memory.
   *
   * ZIM 6 dropped the header's title pointer list and moved the index into the
   * entry `X/listing/titleOrdered/v1`, so it has to be looked up by url first.
   *
   * @return true when a title index is available.
   */
  bool LoadTitleIndex();

  /**
   * @brief Reads the entry index stored at a title-order position.
   * @param position Position in the title index.
   * @param index Output entry index.
   * @return true when the position is valid.
   */
  bool TitleIndexAt(uint32_t position, uint32_t* index);

  /**
   * @brief Reads the file offset of one directory entry.
   * @param index Entry index.
   * @param offset Output offset.
   * @return true when the offset was read.
   */
  bool EntryOffset(uint32_t index, uint64_t* offset);

  /**
   * @brief Makes sure the requested cluster is the one held in the cache.
   * @param cluster Cluster index.
   * @return true when the cache holds that cluster.
   */
  bool EnsureClusterCached(uint32_t cluster);

  SplitFile file_;

  uint32_t entry_count_ = 0;
  uint32_t cluster_count_ = 0;
  uint64_t url_pointer_pos_ = 0;
  uint64_t cluster_pointer_pos_ = 0;
  uint64_t mime_list_pos_ = 0;
  uint64_t checksum_pos_ = 0;
  uint32_t main_page_index_ = 0xFFFFFFFFU;

  // Title-ordered entry indices, held in PSRAM so searching never touches the
  // card: about 4 MB for a million-article Wikipedia.
  uint32_t* title_index_ = nullptr;
  uint32_t title_count_ = 0;

  // Articles that sit next to each other usually share a cluster, so caching
  // the last decompressed one turns most reads into a memcpy.
  uint8_t* cluster_cache_ = nullptr;
  size_t cluster_cache_size_ = 0;
  size_t cluster_cache_capacity_ = 0;
  uint32_t cluster_cache_index_ = 0xFFFFFFFFU;
  bool cluster_cache_wide_offsets_ = false;
};

}  // namespace lilygo_box::survival
