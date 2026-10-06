/**
 * @Description: Writes Motion JPEG video into an AVI container
 * @License: GPL 3.0
 */
#include "survival/avi_writer.h"

#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"

namespace lilygo_box::survival {
namespace {

// Sizes of the fixed structures, from the AVI RIFF specification.
constexpr uint32_t kAvihBytes = 56;
constexpr uint32_t kStrhBytes = 56;
constexpr uint32_t kStrfBytes = 40;
// RIFF(4) + "AVI "(4) + LIST hdrl and its contents + LIST movi header.
constexpr uint32_t kHeaderBytes = 12 + 8 + 4 + 8 + kAvihBytes + 8 + 4 + 8 +
    kStrhBytes + 8 + kStrfBytes + 12;
// AVI offsets are 32-bit, so a file cannot pass 4 GB; recording stops before
// then rather than producing something unplayable. This also keeps it inside
// what FAT32 can hold.
constexpr uint64_t kMaxFileBytes = 3800ULL * 1000 * 1000;
constexpr uint32_t kIndexGrowth = 4096;
// AVIIF_KEYFRAME: every Motion JPEG frame stands alone.
constexpr uint32_t kIndexKeyFrame = 0x10;

// Byte offsets of the header fields that can only be filled in once recording
// stops. They are named rather than written inline because getting one wrong
// silently corrupts a neighbouring field — which is exactly what happened
// while this was being written.
constexpr long kRiffSizeOffset = 4;
constexpr long kAvihMicrosecondsOffset = 32;
constexpr long kAvihMaxBytesPerSecOffset = 36;
constexpr long kAvihTotalFramesOffset = 48;
constexpr long kAvihStreamCountOffset = 56;
constexpr long kAvihSuggestedBufferOffset = 60;
constexpr long kStrhLengthOffset = 140;
constexpr long kStrhSuggestedBufferOffset = 144;

// The main header's fields are consecutive 32-bit words, so the offsets above
// have to be four apart in the order the specification lists them. Stating that
// here turns a mistyped offset into a build failure instead of a corrupted
// neighbouring field.
static_assert(kAvihMaxBytesPerSecOffset == kAvihMicrosecondsOffset + 4);
static_assert(kAvihTotalFramesOffset == kAvihMaxBytesPerSecOffset + 12);
static_assert(kAvihStreamCountOffset == kAvihTotalFramesOffset + 8);
static_assert(kAvihSuggestedBufferOffset == kAvihStreamCountOffset + 4);
static_assert(kStrhSuggestedBufferOffset == kStrhLengthOffset + 4);

/**
 * @brief Writes a little-endian 32-bit value.
 * @param file Destination file.
 * @param value Value to write.
 * @return true when four bytes were written.
 */
bool WriteUint32(FILE* file, uint32_t value) {
  const uint8_t bytes[4] = {
      static_cast<uint8_t>(value),
      static_cast<uint8_t>(value >> 8),
      static_cast<uint8_t>(value >> 16),
      static_cast<uint8_t>(value >> 24),
  };
  return std::fwrite(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
}

/**
 * @brief Writes a four character code.
 * @param file Destination file.
 * @param code Four characters.
 * @return true when four bytes were written.
 */
bool WriteFourCc(FILE* file, const char* code) {
  return std::fwrite(code, 1, 4, file) == 4;
}

/**
 * @brief Overwrites a 32-bit value at a fixed offset.
 * @param file Destination file.
 * @param offset Offset to patch.
 * @param value Value to write.
 * @return true when the patch succeeded.
 */
bool PatchUint32(FILE* file, long offset, uint32_t value) {
  return std::fseek(file, offset, SEEK_SET) == 0 && WriteUint32(file, value);
}

}  // namespace

AviWriter::~AviWriter() { Close(); }

uint32_t AviWriter::duration_ms() const {
  if (frames_per_second_ == 0) {
    return 0;
  }
  return static_cast<uint32_t>(
      static_cast<uint64_t>(frame_count_) * 1000U / frames_per_second_);
}

bool AviWriter::WriteHeader() {
  // Sizes that are only known at the end are written as zero and patched by
  // Close(); the offsets below are fixed by the layout.
  if (!WriteFourCc(file_, "RIFF") || !WriteUint32(file_, 0) ||
      !WriteFourCc(file_, "AVI ")) {
    return false;
  }

  if (!WriteFourCc(file_, "LIST") ||
      !WriteUint32(file_, 4 + 8 + kAvihBytes + 8 + 4 + 8 + kStrhBytes + 8 +
              kStrfBytes) ||
      !WriteFourCc(file_, "hdrl")) {
    return false;
  }

  const uint32_t microseconds_per_frame =
      frames_per_second_ == 0 ? 66666 : 1000000U / frames_per_second_;
  if (!WriteFourCc(file_, "avih") || !WriteUint32(file_, kAvihBytes) ||
      !WriteUint32(file_, microseconds_per_frame) ||
      !WriteUint32(file_, 0) ||           // max bytes per second, patched
      !WriteUint32(file_, 0) ||           // padding granularity
      !WriteUint32(file_, 0x10) ||        // AVIF_HASINDEX
      !WriteUint32(file_, 0) ||           // total frames, patched
      !WriteUint32(file_, 0) ||           // initial frames
      !WriteUint32(file_, 1) ||           // stream count
      !WriteUint32(file_, 0) ||           // suggested buffer size, patched
      !WriteUint32(file_, width_) || !WriteUint32(file_, height_) ||
      !WriteUint32(file_, 0) || !WriteUint32(file_, 0) ||
      !WriteUint32(file_, 0) || !WriteUint32(file_, 0)) {
    return false;
  }

  if (!WriteFourCc(file_, "LIST") ||
      !WriteUint32(file_, 4 + 8 + kStrhBytes + 8 + kStrfBytes) ||
      !WriteFourCc(file_, "strl")) {
    return false;
  }

  if (!WriteFourCc(file_, "strh") || !WriteUint32(file_, kStrhBytes) ||
      !WriteFourCc(file_, "vids") || !WriteFourCc(file_, "MJPG") ||
      !WriteUint32(file_, 0) ||           // flags
      !WriteUint32(file_, 0) ||           // priority and language
      !WriteUint32(file_, 0) ||           // initial frames
      !WriteUint32(file_, 1) ||           // scale
      !WriteUint32(file_, frames_per_second_) ||
      !WriteUint32(file_, 0) ||           // start
      !WriteUint32(file_, 0) ||           // length in frames, patched
      !WriteUint32(file_, 0) ||           // suggested buffer size, patched
      !WriteUint32(file_, 0xFFFFFFFF) ||  // quality: unspecified
      !WriteUint32(file_, 0) ||           // sample size
      !WriteUint32(file_, 0) || !WriteUint32(file_, 0)) {
    return false;
  }

  if (!WriteFourCc(file_, "strf") || !WriteUint32(file_, kStrfBytes) ||
      !WriteUint32(file_, kStrfBytes) ||  // BITMAPINFOHEADER size
      !WriteUint32(file_, width_) || !WriteUint32(file_, height_) ||
      !WriteUint32(file_, 0x00180001) ||  // 1 plane, 24 bits per pixel
      !WriteFourCc(file_, "MJPG") ||
      !WriteUint32(file_, width_ * height_ * 3) ||
      !WriteUint32(file_, 0) || !WriteUint32(file_, 0) ||
      !WriteUint32(file_, 0) || !WriteUint32(file_, 0)) {
    return false;
  }

  if (!WriteFourCc(file_, "LIST") || !WriteUint32(file_, 0) ||
      !WriteFourCc(file_, "movi")) {
    return false;
  }
  movi_start_ = static_cast<uint64_t>(std::ftell(file_));
  return movi_start_ == kHeaderBytes;
}

bool AviWriter::Open(const char* path, uint32_t width, uint32_t height,
    uint32_t frames_per_second) {
  Close();
  if (path == nullptr || width == 0 || height == 0) {
    return false;
  }

  file_ = std::fopen(path, "wb");
  if (file_ == nullptr) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Create video file failed: %s\n", path);
    return false;
  }
  width_ = width;
  height_ = height;
  frames_per_second_ = frames_per_second == 0 ? 15 : frames_per_second;
  frame_count_ = 0;
  largest_frame_ = 0;
  bytes_written_ = 0;

  if (!WriteHeader()) {
    std::fclose(file_);
    file_ = nullptr;
    return false;
  }
  bytes_written_ = movi_start_;
  return true;
}

bool AviWriter::AddFrame(const uint8_t* data, uint32_t size) {
  if (file_ == nullptr || data == nullptr || size == 0) {
    return false;
  }
  // Chunks are word aligned, so an odd frame gets one pad byte.
  const uint32_t padded = size + (size & 1);
  if (bytes_written_ + padded + 8 > kMaxFileBytes) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Video reached the container size limit\n");
    return false;
  }

  if (frame_count_ >= index_capacity_) {
    const uint32_t grown = index_capacity_ + kIndexGrowth;
    auto* offsets = static_cast<uint32_t*>(heap_caps_realloc(index_offsets_,
        grown * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (offsets == nullptr) {
      return false;
    }
    index_offsets_ = offsets;
    auto* sizes = static_cast<uint32_t*>(heap_caps_realloc(index_sizes_,
        grown * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (sizes == nullptr) {
      return false;
    }
    index_sizes_ = sizes;
    index_capacity_ = grown;
  }

  // Index offsets are relative to the start of the movi list's data.
  index_offsets_[frame_count_] =
      static_cast<uint32_t>(bytes_written_ - movi_start_ + 4);
  index_sizes_[frame_count_] = size;

  if (!WriteFourCc(file_, "00dc") || !WriteUint32(file_, size) ||
      std::fwrite(data, 1, size, file_) != size) {
    return false;
  }
  if (padded != size) {
    const uint8_t pad = 0;
    if (std::fwrite(&pad, 1, 1, file_) != 1) {
      return false;
    }
  }

  bytes_written_ += 8 + padded;
  largest_frame_ = size > largest_frame_ ? size : largest_frame_;
  ++frame_count_;
  return true;
}

bool AviWriter::WriteIndex() {
  if (!WriteFourCc(file_, "idx1") ||
      !WriteUint32(file_, frame_count_ * 16)) {
    return false;
  }
  for (uint32_t index = 0; index < frame_count_; ++index) {
    if (!WriteFourCc(file_, "00dc") ||
        !WriteUint32(file_, kIndexKeyFrame) ||
        !WriteUint32(file_, index_offsets_[index]) ||
        !WriteUint32(file_, index_sizes_[index])) {
      return false;
    }
  }
  return true;
}

bool AviWriter::Close() {
  if (file_ == nullptr) {
    heap_caps_free(index_offsets_);
    heap_caps_free(index_sizes_);
    index_offsets_ = nullptr;
    index_sizes_ = nullptr;
    index_capacity_ = 0;
    return true;
  }

  bool ok = true;
  const uint64_t movi_bytes = bytes_written_ - movi_start_;
  if (frame_count_ > 0) {
    ok = WriteIndex();
  }
  const uint64_t total = static_cast<uint64_t>(std::ftell(file_));

  // Patch every size that was only known once recording finished.
  const uint32_t bytes_per_second = duration_ms() == 0
      ? 0
      : static_cast<uint32_t>(movi_bytes * 1000U / duration_ms());
  ok = ok && PatchUint32(
      file_, kRiffSizeOffset, static_cast<uint32_t>(total - 8));
  ok = ok && PatchUint32(file_, kAvihMaxBytesPerSecOffset, bytes_per_second);
  ok = ok && PatchUint32(file_, kAvihTotalFramesOffset, frame_count_);
  ok = ok && PatchUint32(file_, kAvihSuggestedBufferOffset, largest_frame_);
  ok = ok && PatchUint32(file_, kStrhLengthOffset, frame_count_);
  ok = ok && PatchUint32(file_, kStrhSuggestedBufferOffset, largest_frame_);
  // The movi list carries its own size, four bytes before its fourcc.
  ok = ok && PatchUint32(file_, static_cast<long>(movi_start_) - 8,
      static_cast<uint32_t>(movi_bytes + 4));

  if (std::fclose(file_) != 0) {
    ok = false;
  }
  file_ = nullptr;
  heap_caps_free(index_offsets_);
  heap_caps_free(index_sizes_);
  index_offsets_ = nullptr;
  index_sizes_ = nullptr;
  index_capacity_ = 0;
  if (!ok) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Finalising the video file failed\n");
  }
  return ok;
}

}  // namespace lilygo_box::survival
