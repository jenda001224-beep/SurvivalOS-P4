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
// WAVEFORMATEX with an empty extension.
constexpr uint32_t kAudioStrfBytes = 18;
constexpr uint32_t kVideoListBytes = 4 + 8 + kStrhBytes + 8 + kStrfBytes;
constexpr uint32_t kAudioListBytes = 4 + 8 + kStrhBytes + 8 + kAudioStrfBytes;
// Word alignment after the odd-sized audio format.
constexpr uint32_t kAudioFormatPadBytes = kAudioStrfBytes & 1;
// RIFF(4) + "AVI "(4) + LIST hdrl and its contents + LIST movi header.
constexpr uint32_t kHeaderBytes = 12 + 8 + 4 + 8 + kAvihBytes + 8 +
    kVideoListBytes + 12;
// The reader seeks with a 32-bit long, so a file has to stay below 2 GiB to
// play back on the phone; recording stops there, with room left for the index.
constexpr uint64_t kMaxFileBytes = 2000ULL * 1000 * 1000;
constexpr uint32_t kIndexGrowth = 4096;
// AVIIF_KEYFRAME: every Motion JPEG frame stands alone.
constexpr uint32_t kIndexKeyFrame = 0x10;
// Marks an index entry as audio; no chunk comes near 2 GiB, so the top bit of
// its size is free.
constexpr uint32_t kIndexAudioFlag = 0x80000000U;

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
constexpr long kStrhScaleOffset = 128;
constexpr long kStrhRateOffset = 132;
constexpr long kStrhLengthOffset = 140;
constexpr long kStrhSuggestedBufferOffset = 144;
// The audio stream header follows the video stream's list.
constexpr long kAudioStrhOffset = 24 + 8 + kAvihBytes + 8 + kVideoListBytes +
    12 + 8;
constexpr long kAudioStrhLengthOffset = kAudioStrhOffset + 32;
constexpr long kAudioStrhSuggestedBufferOffset = kAudioStrhOffset + 36;

// The main header's fields are consecutive 32-bit words, so the offsets above
// have to be four apart in the order the specification lists them. Stating that
// here turns a mistyped offset into a build failure instead of a corrupted
// neighbouring field.
static_assert(kAvihMaxBytesPerSecOffset == kAvihMicrosecondsOffset + 4);
static_assert(kAvihTotalFramesOffset == kAvihMaxBytesPerSecOffset + 12);
static_assert(kAvihStreamCountOffset == kAvihTotalFramesOffset + 8);
static_assert(kAvihSuggestedBufferOffset == kAvihStreamCountOffset + 4);
static_assert(kStrhSuggestedBufferOffset == kStrhLengthOffset + 4);
static_assert(kStrhRateOffset == kStrhScaleOffset + 4);
static_assert(kStrhLengthOffset == kStrhRateOffset + 8);

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

uint32_t AviWriter::audio_duration_ms() const {
  const uint32_t byte_rate = audio_sample_rate_hz_ * audio_block_bytes();
  if (byte_rate == 0) {
    return 0;
  }
  return static_cast<uint32_t>(audio_bytes_ * 1000U / byte_rate);
}

bool AviWriter::WriteHeader() {
  // Sizes that are only known at the end are written as zero and patched by
  // Close(); the offsets below are fixed by the layout.
  if (!WriteFourCc(file_, "RIFF") || !WriteUint32(file_, 0) ||
      !WriteFourCc(file_, "AVI ")) {
    return false;
  }

  if (!WriteFourCc(file_, "LIST") ||
      !WriteUint32(file_, 4 + 8 + kAvihBytes + 8 + kVideoListBytes +
              (audio_channel_count_ == 0
                      ? 0
                      : 8 + kAudioListBytes + kAudioFormatPadBytes)) ||
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
      !WriteUint32(file_, audio_channel_count_ == 0 ? 1 : 2) ||  // streams
      !WriteUint32(file_, 0) ||           // suggested buffer size, patched
      !WriteUint32(file_, width_) || !WriteUint32(file_, height_) ||
      !WriteUint32(file_, 0) || !WriteUint32(file_, 0) ||
      !WriteUint32(file_, 0) || !WriteUint32(file_, 0)) {
    return false;
  }

  if (!WriteFourCc(file_, "LIST") ||
      !WriteUint32(file_, kVideoListBytes) ||
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

  if (audio_channel_count_ != 0 && !WriteAudioStreamHeader()) {
    return false;
  }

  if (!WriteFourCc(file_, "LIST") || !WriteUint32(file_, 0) ||
      !WriteFourCc(file_, "movi")) {
    return false;
  }
  movi_start_ = static_cast<uint64_t>(std::ftell(file_));
  return movi_start_ == kHeaderBytes +
      (audio_channel_count_ == 0
              ? 0
              : 8 + kAudioListBytes + kAudioFormatPadBytes);
}

bool AviWriter::WriteAudioStreamHeader() {
  const uint32_t block = audio_block_bytes();
  const uint32_t byte_rate = audio_sample_rate_hz_ * block;
  if (!WriteFourCc(file_, "LIST") ||
      !WriteUint32(file_, kAudioListBytes + kAudioFormatPadBytes) ||
      !WriteFourCc(file_, "strl")) {
    return false;
  }
  if (!WriteFourCc(file_, "strh") || !WriteUint32(file_, kStrhBytes) ||
      !WriteFourCc(file_, "auds") || !WriteUint32(file_, 0) ||
      !WriteUint32(file_, 0) ||           // flags
      !WriteUint32(file_, 0) ||           // priority and language
      !WriteUint32(file_, 0) ||           // initial frames
      !WriteUint32(file_, block) ||       // scale
      !WriteUint32(file_, byte_rate) ||   // rate: scale/rate is one sample
      !WriteUint32(file_, 0) ||           // start
      !WriteUint32(file_, 0) ||           // length in samples, patched
      !WriteUint32(file_, 0) ||           // suggested buffer size, patched
      !WriteUint32(file_, 0xFFFFFFFF) ||  // quality: unspecified
      !WriteUint32(file_, block) ||       // sample size
      !WriteUint32(file_, 0) || !WriteUint32(file_, 0)) {
    return false;
  }
  const uint8_t format[kAudioStrfBytes] = {
      1, 0,  // PCM
      audio_channel_count_, 0,
      static_cast<uint8_t>(audio_sample_rate_hz_),
      static_cast<uint8_t>(audio_sample_rate_hz_ >> 8),
      static_cast<uint8_t>(audio_sample_rate_hz_ >> 16),
      static_cast<uint8_t>(audio_sample_rate_hz_ >> 24),
      static_cast<uint8_t>(byte_rate),
      static_cast<uint8_t>(byte_rate >> 8),
      static_cast<uint8_t>(byte_rate >> 16),
      static_cast<uint8_t>(byte_rate >> 24),
      static_cast<uint8_t>(block), 0,
      static_cast<uint8_t>(audio_bits_per_sample_), 0,
      0, 0,  // no extension
  };
  if (!WriteFourCc(file_, "strf") || !WriteUint32(file_, kAudioStrfBytes) ||
      std::fwrite(format, 1, sizeof(format), file_) != sizeof(format)) {
    return false;
  }
  const uint8_t pad = 0;
  return kAudioFormatPadBytes == 0 || std::fwrite(&pad, 1, 1, file_) == 1;
}

bool AviWriter::GrowIndexIfFull() {
  if (index_count_ < index_capacity_) {
    return true;
  }
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
  return true;
}

bool AviWriter::Open(const char* path, uint32_t width, uint32_t height,
    uint32_t frames_per_second, uint32_t audio_sample_rate_hz,
    uint8_t audio_channel_count, uint8_t audio_bits_per_sample) {
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
  index_count_ = 0;
  audio_sample_rate_hz_ = audio_sample_rate_hz;
  audio_channel_count_ = audio_sample_rate_hz == 0 ? 0 : audio_channel_count;
  audio_bits_per_sample_ = audio_bits_per_sample;
  audio_bytes_ = 0;
  largest_audio_chunk_ = 0;
  microseconds_per_frame_ = 0;

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

  if (!GrowIndexIfFull()) {
    return false;
  }

  // Index offsets are relative to the start of the movi list's data.
  index_offsets_[index_count_] =
      static_cast<uint32_t>(bytes_written_ - movi_start_ + 4);
  index_sizes_[index_count_] = size;

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
  ++index_count_;
  return true;
}

bool AviWriter::AddAudio(const uint8_t* data, uint32_t size) {
  if (file_ == nullptr || data == nullptr || size == 0 ||
      audio_channel_count_ == 0) {
    return false;
  }
  const uint32_t padded = size + (size & 1);
  if (bytes_written_ + padded + 8 > kMaxFileBytes || !GrowIndexIfFull()) {
    return false;
  }
  index_offsets_[index_count_] =
      static_cast<uint32_t>(bytes_written_ - movi_start_ + 4);
  index_sizes_[index_count_] = size | kIndexAudioFlag;

  if (!WriteFourCc(file_, "01wb") || !WriteUint32(file_, size) ||
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
  audio_bytes_ += size;
  largest_audio_chunk_ =
      size > largest_audio_chunk_ ? size : largest_audio_chunk_;
  ++index_count_;
  return true;
}

bool AviWriter::WriteIndex() {
  if (!WriteFourCc(file_, "idx1") ||
      !WriteUint32(file_, index_count_ * 16)) {
    return false;
  }
  for (uint32_t index = 0; index < index_count_; ++index) {
    const bool audio = (index_sizes_[index] & kIndexAudioFlag) != 0;
    if (!WriteFourCc(file_, audio ? "01wb" : "00dc") ||
        !WriteUint32(file_, kIndexKeyFrame) ||
        !WriteUint32(file_, index_offsets_[index]) ||
        !WriteUint32(file_, index_sizes_[index] & ~kIndexAudioFlag)) {
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
  if (index_count_ > 0) {
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
  if (microseconds_per_frame_ != 0) {
    // The real pace of the recording, so picture and sound stay together.
    ok = ok && PatchUint32(file_, kAvihMicrosecondsOffset, microseconds_per_frame_);
    ok = ok && PatchUint32(file_, kStrhScaleOffset, microseconds_per_frame_);
    ok = ok && PatchUint32(file_, kStrhRateOffset, 1000000U);
  }
  if (audio_channel_count_ != 0) {
    ok = ok && PatchUint32(file_, kAudioStrhLengthOffset,
        static_cast<uint32_t>(audio_bytes_ / audio_block_bytes()));
    ok = ok && PatchUint32(
        file_, kAudioStrhSuggestedBufferOffset, largest_audio_chunk_);
  }
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
