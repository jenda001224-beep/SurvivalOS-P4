/**
 * @Description: Random access across a dataset split into FAT32-sized parts
 * @License: GPL 3.0
 */
#include "survival/split_file.h"

#include <cstdio>
#include <cstring>
#include <new>

#include "base/logger.h"
#include "ff.h"

namespace lilygo_box::survival {
namespace {

constexpr size_t kMaxPathLength = 192;

/**
 * @brief Builds the name of the n-th part, the way `split` names them.
 * @param base Base path without a suffix.
 * @param index Part index.
 * @param buffer Output buffer.
 * @param size Buffer size.
 * @return true when the name fitted.
 */
bool BuildPartPath(
    const char* base, int index, char* buffer, size_t size) {
  const char first = static_cast<char>('a' + index / 26);
  const char second = static_cast<char>('a' + index % 26);
  return std::snprintf(buffer, size, "%s%c%c", base, first, second) <
      static_cast<int>(size);
}

}  // namespace

SplitFile::~SplitFile() {
  Close();
  for (void*& handle : fatfs_files_) {
    delete static_cast<FIL*>(handle);
    handle = nullptr;
  }
}

bool SplitFile::ToFatfsPath(
    const char* path, char* buffer, size_t size) const {
  return std::snprintf(buffer, size, "%s%s", fatfs_drive_,
             path + vfs_prefix_length_) < static_cast<int>(size);
}

bool SplitFile::ResolveFatfsDrive(const char* path) {
  use_fatfs_ = false;
  // "/sdcard/wiki/a.zim": the mount point is everything before the second
  // slash, and FatFs knows the rest under a drive number.
  const char* relative = path[0] == '/' ? std::strchr(path + 1, '/') : nullptr;
  if (relative == nullptr) {
    return false;
  }
  vfs_prefix_length_ = static_cast<size_t>(relative - path);
  for (int drive = 0; drive < FF_VOLUMES; ++drive) {
    std::snprintf(fatfs_drive_, sizeof(fatfs_drive_), "%d:", drive);
    char fatfs_path[kMaxPathLength + 4] = {};
    FILINFO info = {};
    if (ToFatfsPath(path, fatfs_path, sizeof(fatfs_path)) &&
        f_stat(fatfs_path, &info) == FR_OK && (info.fattrib & AM_DIR) == 0) {
      use_fatfs_ = true;
      return true;
    }
  }
  fatfs_drive_[0] = '\0';
  return false;
}

bool SplitFile::MeasureFile(const char* path, uint64_t* size) const {
  if (use_fatfs_) {
    char fatfs_path[kMaxPathLength + 4] = {};
    FILINFO info = {};
    if (!ToFatfsPath(path, fatfs_path, sizeof(fatfs_path)) ||
        f_stat(fatfs_path, &info) != FR_OK) {
      return false;
    }
    *size = static_cast<uint64_t>(info.fsize);
    return true;
  }
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  long length = -1;
  if (std::fseek(file, 0, SEEK_END) == 0) {
    length = std::ftell(file);
  }
  std::fclose(file);
  if (length < 0) {
    return false;
  }
  *size = static_cast<uint64_t>(length);
  return true;
}

bool SplitFile::AddPart(const char* path) {
  if (part_count_ >= kMaxParts) {
    return false;
  }
  uint64_t size = 0;
  if (!MeasureFile(path, &size) || size == 0) {
    return false;
  }

  part_sizes_[part_count_] = size;
  part_offsets_[part_count_] = total_size_;
  total_size_ += size;
  ++part_count_;
  return true;
}

bool SplitFile::Open(const char* path) {
  Close();
  if (path == nullptr || path[0] == '\0' ||
      std::strlen(path) + 3 >= kMaxPathLength) {
    return false;
  }
  std::snprintf(base_path_, sizeof(base_path_), "%s", path);

  // Prefer the split parts: if `data.zimaa` exists the caller almost certainly
  // means the split dataset even when `data.zim` also happens to be there.
  char part_path[kMaxPathLength] = {};
  const bool split = BuildPartPath(base_path_, 0, part_path, sizeof(part_path)) &&
      ResolveFatfsDrive(part_path);
  if (!split) {
    ResolveFatfsDrive(base_path_);
  }
  if (split && AddPart(part_path)) {
    single_file_ = false;
    for (int index = 1; index < kMaxParts; ++index) {
      if (!BuildPartPath(base_path_, index, part_path, sizeof(part_path)) ||
          !AddPart(part_path)) {
        break;
      }
    }
    LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
        "Opened %d part(s) of %s, %llu bytes%s\n", part_count_, base_path_,
        static_cast<unsigned long long>(total_size_),
        use_fatfs_ ? " (FatFs)" : "");
    return true;
  }

  if (AddPart(base_path_)) {
    single_file_ = true;
    LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
        "Opened %s, %llu bytes%s\n", base_path_,
        static_cast<unsigned long long>(total_size_),
        use_fatfs_ ? " (FatFs)" : "");
    return true;
  }
  LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
      "Open dataset failed: %s\n", base_path_);
  base_path_[0] = '\0';
  return false;
}

int SplitFile::HandleFor(int index) {
  if (index < 0 || index >= part_count_) {
    return -1;
  }
  for (int slot = 0; slot < open_count_; ++slot) {
    if (open_indices_[slot] != index) {
      continue;
    }
    // Move it to the front so the least recently used one is always last.
    FILE* file = open_files_[slot];
    void* fatfs_file = fatfs_files_[slot];
    for (int move = slot; move > 0; --move) {
      open_files_[move] = open_files_[move - 1];
      fatfs_files_[move] = fatfs_files_[move - 1];
      open_indices_[move] = open_indices_[move - 1];
    }
    open_files_[0] = file;
    fatfs_files_[0] = fatfs_file;
    open_indices_[0] = index;
    return 0;
  }

  char path[kMaxPathLength] = {};
  if (single_file_) {
    std::snprintf(path, sizeof(path), "%s", base_path_);
  } else if (!BuildPartPath(base_path_, index, path, sizeof(path))) {
    return -1;
  }

  // Free the least recently used slot when the pool is full. Its FatFs
  // object is kept and reused, which saves a 4 KB allocation per reopen.
  if (open_count_ == kMaxOpenParts) {
    const int last = kMaxOpenParts - 1;
    if (open_files_[last] != nullptr) {
      std::fclose(open_files_[last]);
      open_files_[last] = nullptr;
    }
    if (fatfs_files_[last] != nullptr) {
      f_close(static_cast<FIL*>(fatfs_files_[last]));
    }
    --open_count_;
  }
  void* spare_fatfs_file = fatfs_files_[open_count_];
  for (int move = open_count_; move > 0; --move) {
    open_files_[move] = open_files_[move - 1];
    fatfs_files_[move] = fatfs_files_[move - 1];
    open_indices_[move] = open_indices_[move - 1];
  }
  open_files_[0] = nullptr;
  fatfs_files_[0] = spare_fatfs_file;
  open_indices_[0] = index;

  bool opened = false;
  if (use_fatfs_) {
    if (fatfs_files_[0] == nullptr) {
      fatfs_files_[0] = new (std::nothrow) FIL();
    }
    char fatfs_path[kMaxPathLength + 4] = {};
    opened = fatfs_files_[0] != nullptr &&
        ToFatfsPath(path, fatfs_path, sizeof(fatfs_path)) &&
        f_open(static_cast<FIL*>(fatfs_files_[0]), fatfs_path,
            FA_READ | FA_OPEN_EXISTING) == FR_OK;
  } else {
    open_files_[0] = std::fopen(path, "rb");
    opened = open_files_[0] != nullptr;
  }
  if (!opened) {
    // Undo the shift so the pool stays consistent; the FatFs object that was
    // meant for this part goes back to being the spare.
    void* unused_fatfs_file = fatfs_files_[0];
    for (int move = 0; move < open_count_; ++move) {
      open_files_[move] = open_files_[move + 1];
      fatfs_files_[move] = fatfs_files_[move + 1];
      open_indices_[move] = open_indices_[move + 1];
    }
    open_files_[open_count_] = nullptr;
    fatfs_files_[open_count_] = unused_fatfs_file;
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Reopen part failed: %s\n", path);
    return -1;
  }
  ++open_count_;
  return 0;
}

void SplitFile::CloseHandles() {
  for (int slot = 0; slot < open_count_; ++slot) {
    if (open_files_[slot] != nullptr) {
      std::fclose(open_files_[slot]);
      open_files_[slot] = nullptr;
    }
    if (fatfs_files_[slot] != nullptr) {
      f_close(static_cast<FIL*>(fatfs_files_[slot]));
    }
  }
  open_count_ = 0;
}

void SplitFile::Close() {
  CloseHandles();
  for (int index = 0; index < part_count_; ++index) {
    part_sizes_[index] = 0;
    part_offsets_[index] = 0;
  }
  part_count_ = 0;
  total_size_ = 0;
  single_file_ = false;
  use_fatfs_ = false;
  fatfs_drive_[0] = '\0';
  vfs_prefix_length_ = 0;
  base_path_[0] = '\0';
}

bool SplitFile::ReadAt(uint64_t offset, void* buffer, size_t size) {
  if (part_count_ == 0 || buffer == nullptr) {
    return false;
  }
  if (size == 0) {
    return true;
  }
  if (offset > total_size_ || total_size_ - offset < size) {
    return false;
  }

  auto* out = static_cast<uint8_t*>(buffer);
  size_t remaining = size;
  uint64_t position = offset;

  while (remaining > 0) {
    // Parts are few, so a linear scan beats keeping a second index around.
    int part = part_count_ - 1;
    while (part > 0 && part_offsets_[part] > position) {
      --part;
    }
    const uint64_t local = position - part_offsets_[part];
    if (local >= part_sizes_[part]) {
      return false;
    }
    const uint64_t available = part_sizes_[part] - local;
    const size_t chunk = static_cast<size_t>(
        available < remaining ? available : remaining);

    const int slot = HandleFor(part);
    if (slot < 0) {
      return false;
    }
    if (use_fatfs_) {
      auto* file = static_cast<FIL*>(fatfs_files_[slot]);
      UINT read = 0;
      if (f_lseek(file, static_cast<FSIZE_t>(local)) != FR_OK ||
          f_read(file, out, static_cast<UINT>(chunk), &read) != FR_OK ||
          read != chunk) {
        return false;
      }
    } else {
      FILE* file = open_files_[slot];
      if (std::fseek(file, static_cast<long>(local), SEEK_SET) != 0 ||
          std::fread(out, 1, chunk, file) != chunk) {
        return false;
      }
    }
    out += chunk;
    position += chunk;
    remaining -= chunk;
  }
  return true;
}

bool SplitFile::ReadUint16(uint64_t offset, uint16_t* value) {
  uint8_t raw[2] = {};
  if (value == nullptr || !ReadAt(offset, raw, sizeof(raw))) {
    return false;
  }
  *value = static_cast<uint16_t>(
      static_cast<uint16_t>(raw[0]) | (static_cast<uint16_t>(raw[1]) << 8));
  return true;
}

bool SplitFile::ReadUint32(uint64_t offset, uint32_t* value) {
  uint8_t raw[4] = {};
  if (value == nullptr || !ReadAt(offset, raw, sizeof(raw))) {
    return false;
  }
  *value = static_cast<uint32_t>(raw[0]) |
      (static_cast<uint32_t>(raw[1]) << 8) |
      (static_cast<uint32_t>(raw[2]) << 16) |
      (static_cast<uint32_t>(raw[3]) << 24);
  return true;
}

bool SplitFile::ReadUint64(uint64_t offset, uint64_t* value) {
  uint8_t raw[8] = {};
  if (value == nullptr || !ReadAt(offset, raw, sizeof(raw))) {
    return false;
  }
  uint64_t result = 0;
  for (int index = 7; index >= 0; --index) {
    result = (result << 8) | raw[index];
  }
  *value = result;
  return true;
}

}  // namespace lilygo_box::survival
