/**
 * @Description: Random access across a dataset split into FAT32-sized parts
 * @License: GPL 3.0
 */
#include "survival/split_file.h"

#include <cstdio>
#include <cstring>

#include "base/logger.h"

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

SplitFile::~SplitFile() { Close(); }

bool SplitFile::AddPart(const char* path) {
  if (part_count_ >= kMaxParts) {
    return false;
  }
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  long size = -1;
  if (std::fseek(file, 0, SEEK_END) == 0) {
    size = std::ftell(file);
  }
  std::fclose(file);
  if (size <= 0) {
    return false;
  }

  part_sizes_[part_count_] = static_cast<uint64_t>(size);
  part_offsets_[part_count_] = total_size_;
  total_size_ += static_cast<uint64_t>(size);
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
  if (BuildPartPath(base_path_, 0, part_path, sizeof(part_path)) &&
      AddPart(part_path)) {
    single_file_ = false;
    for (int index = 1; index < kMaxParts; ++index) {
      if (!BuildPartPath(base_path_, index, part_path, sizeof(part_path)) ||
          !AddPart(part_path)) {
        break;
      }
    }
    LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
        "Opened %d part(s) of %s, %llu bytes\n", part_count_, base_path_,
        static_cast<unsigned long long>(total_size_));
    return true;
  }

  if (AddPart(base_path_)) {
    single_file_ = true;
    return true;
  }
  LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
      "Open dataset failed: %s\n", base_path_);
  base_path_[0] = '\0';
  return false;
}

FILE* SplitFile::HandleFor(int index) {
  if (index < 0 || index >= part_count_) {
    return nullptr;
  }
  for (int slot = 0; slot < open_count_; ++slot) {
    if (open_indices_[slot] != index) {
      continue;
    }
    // Move it to the front so the least recently used one is always last.
    FILE* file = open_files_[slot];
    for (int move = slot; move > 0; --move) {
      open_files_[move] = open_files_[move - 1];
      open_indices_[move] = open_indices_[move - 1];
    }
    open_files_[0] = file;
    open_indices_[0] = index;
    return file;
  }

  char path[kMaxPathLength] = {};
  if (single_file_) {
    std::snprintf(path, sizeof(path), "%s", base_path_);
  } else if (!BuildPartPath(base_path_, index, path, sizeof(path))) {
    return nullptr;
  }
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Reopen part failed: %s\n", path);
    return nullptr;
  }

  if (open_count_ == kMaxOpenParts) {
    std::fclose(open_files_[kMaxOpenParts - 1]);
    --open_count_;
  }
  for (int move = open_count_; move > 0; --move) {
    open_files_[move] = open_files_[move - 1];
    open_indices_[move] = open_indices_[move - 1];
  }
  open_files_[0] = file;
  open_indices_[0] = index;
  ++open_count_;
  return file;
}

void SplitFile::CloseHandles() {
  for (int slot = 0; slot < open_count_; ++slot) {
    if (open_files_[slot] != nullptr) {
      std::fclose(open_files_[slot]);
      open_files_[slot] = nullptr;
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

    FILE* file = HandleFor(part);
    if (file == nullptr ||
        std::fseek(file, static_cast<long>(local), SEEK_SET) != 0 ||
        std::fread(out, 1, chunk, file) != chunk) {
      return false;
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
