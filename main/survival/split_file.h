/**
 * @Description: Random access across a dataset split into FAT32-sized parts
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>
#include <cstdio>

namespace lilygo_box::survival {

// ESP-IDF's FatFs has exFAT disabled, so no file on the SD card may exceed
// 4 GB. Large datasets are therefore stored the way `split` writes them —
// `planet.pmtilesaa`, `planet.pmtilesab`, … — and this class stitches the parts
// back into one addressable byte range.
//
// Parts are opened lazily and only a few handles are kept: the SD card is
// mounted with room for five open files, while the planet basemap alone is 67
// parts. Holding them all open would fail on the sixth.
//
// A single unsplit file works too: pass its path and it is used as part zero.
class SplitFile {
 public:
  // At 2 GB per part this covers datasets up to 512 GB.
  static constexpr int kMaxParts = 256;
  // How many part handles stay open at once. Reads walk sequentially far more
  // often than they jump, so a handful covers almost every access without
  // reopening.
  static constexpr int kMaxOpenParts = 3;

  SplitFile() = default;
  ~SplitFile();

  SplitFile(const SplitFile&) = delete;
  SplitFile& operator=(const SplitFile&) = delete;

  /**
   * @brief Opens a dataset, split or not.
   * @param path Path to the whole file, or to its base name without the part
   *        suffix (both `data.zim` and `data.zim` for `data.zimaa` work).
   * @return true when at least one part was opened.
   */
  bool Open(const char* path);

  /**
   * @brief Closes every part.
   */
  void Close();

  /**
   * @brief Reports whether the dataset is open.
   * @return true when open.
   */
  bool IsOpen() const { return part_count_ > 0; }

  /**
   * @brief Total size of all parts together.
   * @return Size in bytes.
   */
  uint64_t Size() const { return total_size_; }

  /**
   * @brief Reads a byte range, crossing part boundaries as needed.
   * @param offset Offset into the logical dataset.
   * @param buffer Destination buffer.
   * @param size Number of bytes to read.
   * @return true when every requested byte was read.
   */
  bool ReadAt(uint64_t offset, void* buffer, size_t size);

  /**
   * @brief Reads a little-endian unsigned integer.
   * @param offset Offset into the logical dataset.
   * @param value Output value.
   * @return true when the value was read.
   */
  bool ReadUint32(uint64_t offset, uint32_t* value);
  bool ReadUint64(uint64_t offset, uint64_t* value);
  bool ReadUint16(uint64_t offset, uint16_t* value);

 private:
  /**
   * @brief Measures one part and records where it sits in the dataset.
   * @param path Path of the part.
   * @return true when the part exists and has content.
   */
  bool AddPart(const char* path);

  /**
   * @brief Returns an open handle for a part, opening it if needed.
   * @param index Part index.
   * @return The handle, or nullptr when it could not be opened.
   */
  FILE* HandleFor(int index);

  /**
   * @brief Closes every open handle, keeping the part table.
   */
  void CloseHandles();

  // Base path the part names are built from.
  char base_path_[192] = {};
  // Set when the dataset is a single unsplit file.
  bool single_file_ = false;

  // Size of each part.
  uint64_t part_sizes_[kMaxParts] = {};
  // Logical offset each part starts at.
  uint64_t part_offsets_[kMaxParts] = {};
  int part_count_ = 0;
  uint64_t total_size_ = 0;

  // The small pool of open handles, most recently used first.
  FILE* open_files_[kMaxOpenParts] = {};
  int open_indices_[kMaxOpenParts] = {};
  int open_count_ = 0;
};

}  // namespace lilygo_box::survival
