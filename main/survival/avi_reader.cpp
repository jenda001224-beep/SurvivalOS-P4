/**
 * @Description: Reads Motion JPEG video out of an AVI container
 * @License: GPL 3.0
 */
#include "survival/avi_reader.h"

#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"

namespace lilygo_box::survival {
namespace {

// A video with more frames than this is longer than anything this device is
// going to record, and the index alone would cost megabytes.
constexpr uint32_t kMaxFrames = 200000;
// The index is read in blocks: one read per entry takes seconds on a long video.
constexpr uint32_t kIndexBlockEntries = 256;
constexpr uint16_t kWaveFormatPcm = 1;
// Videos recorded before frame times were measured all say 15 fps, but the
// camera delivered about 10 (measured on the device), so they played half as
// fast again as they should. Exactly this value, with no sound, marks one.
constexpr uint32_t kLegacyMicrosecondsPerFrame = 66666;
constexpr uint32_t kLegacyRealMicrosecondsPerFrame = 100000;

/**
 * @brief Reads a little-endian 32-bit value from a buffer.
 * @param data Four bytes.
 * @return The value.
 */
uint32_t ReadUint32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
      (static_cast<uint32_t>(data[1]) << 8) |
      (static_cast<uint32_t>(data[2]) << 16) |
      (static_cast<uint32_t>(data[3]) << 24);
}

/**
 * @brief Reads a fixed number of bytes at an offset.
 * @param file File to read.
 * @param offset Offset to read from.
 * @param buffer Destination buffer.
 * @param size Number of bytes.
 * @return true when every byte was read.
 */
bool ReadAt(FILE* file, long offset, void* buffer, size_t size) {
  return std::fseek(file, offset, SEEK_SET) == 0 &&
      std::fread(buffer, 1, size, file) == size;
}

}  // namespace

AviReader::~AviReader() { Close(); }

uint32_t AviReader::duration_ms() const {
  if (frames_per_second_ == 0) {
    return 0;
  }
  return static_cast<uint32_t>(static_cast<uint64_t>(frame_count_) *
      microseconds_per_frame_ / 1000U);
}

bool AviReader::Open(const char* path) {
  Close();
  if (path == nullptr) {
    return false;
  }
  file_ = std::fopen(path, "rb");
  if (file_ == nullptr) {
    return false;
  }

  uint8_t header[12] = {};
  if (!ReadAt(file_, 0, header, sizeof(header)) ||
      std::memcmp(header, "RIFF", 4) != 0 ||
      std::memcmp(header + 8, "AVI ", 4) != 0) {
    Close();
    return false;
  }

  // Walk the top-level chunks rather than trusting fixed offsets: other
  // recorders lay their headers out differently.
  long movi_position = -1;
  long index_position = -1;
  uint32_t index_bytes = 0;
  long position = 12;
  std::fseek(file_, 0, SEEK_END);
  const long file_size = std::ftell(file_);

  while (position + 8 <= file_size) {
    uint8_t chunk[12] = {};
    if (!ReadAt(file_, position, chunk, 8)) {
      break;
    }
    const uint32_t size = ReadUint32(chunk + 4);
    if (std::memcmp(chunk, "LIST", 4) == 0) {
      if (!ReadAt(file_, position + 8, chunk + 8, 4)) {
        break;
      }
      if (std::memcmp(chunk + 8, "movi", 4) == 0) {
        movi_position = position + 8;
      } else if (std::memcmp(chunk + 8, "hdrl", 4) == 0) {
        // avih sits first inside hdrl; its frame rate and size are all that
        // is needed to play the file back.
        uint8_t avih[68] = {};
        if (ReadAt(file_, position + 12, avih, sizeof(avih)) &&
            std::memcmp(avih, "avih", 4) == 0) {
          const uint32_t microseconds = ReadUint32(avih + 8);
          if (microseconds > 0) {
            microseconds_per_frame_ = microseconds;
            frames_per_second_ = (1000000U + microseconds / 2) / microseconds;
          }
          frame_count_ = ReadUint32(avih + 24);
          width_ = ReadUint32(avih + 40);
          height_ = ReadUint32(avih + 44);
        }
        ReadAudioFormat(position + 12, position + 8 + size);
      }
      position += 8 + size + (size & 1);
      continue;
    }
    if (std::memcmp(chunk, "idx1", 4) == 0) {
      index_position = position + 8;
      index_bytes = size;
    }
    position += 8 + size + (size & 1);
  }

  if (movi_position < 0 || index_position < 0 || index_bytes < 16) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Video has no index: %s\n", path);
    Close();
    return false;
  }

  // This phone names its videos VID_<date>-<time>.avi.
  const char* file_name = std::strrchr(path, '/');
  const bool recorded_by_this_phone = std::strncmp(
      file_name == nullptr ? path : file_name + 1, "VID_", 4) == 0;

  const uint32_t entries = index_bytes / 16;
  if (entries == 0 || entries > kMaxFrames) {
    Close();
    return false;
  }
  frame_offsets_ = static_cast<uint32_t*>(heap_caps_malloc(
      entries * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  frame_sizes_ = static_cast<uint32_t*>(heap_caps_malloc(
      entries * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  const bool wants_audio = audio_channel_count_ != 0;
  if (wants_audio) {
    audio_offsets_ = static_cast<uint32_t*>(heap_caps_malloc(
        entries * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    audio_sizes_ = static_cast<uint32_t*>(heap_caps_malloc(
        entries * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  auto* block = static_cast<uint8_t*>(heap_caps_malloc(
      kIndexBlockEntries * 16, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (frame_offsets_ == nullptr || frame_sizes_ == nullptr || block == nullptr ||
      (wants_audio && (audio_offsets_ == nullptr || audio_sizes_ == nullptr))) {
    heap_caps_free(block);
    Close();
    return false;
  }

  uint32_t kept = 0;
  std::fseek(file_, index_position, SEEK_SET);
  for (uint32_t index = 0; index < entries; index += kIndexBlockEntries) {
    const uint32_t wanted = entries - index < kIndexBlockEntries
        ? entries - index
        : kIndexBlockEntries;
    const uint32_t got =
        static_cast<uint32_t>(std::fread(block, 16, wanted, file_));
    for (uint32_t item = 0; item < got; ++item) {
      const uint8_t* entry = block + item * 16;
      const uint32_t offset = ReadUint32(entry + 8);
      const uint32_t size = ReadUint32(entry + 12);
      if (size == 0) {
        continue;
      }
      // Index offsets are relative to the 'movi' fourcc and point at the chunk
      // header, so the payload starts eight bytes later.
      const uint32_t payload =
          static_cast<uint32_t>(movi_position) + offset + 8;
      if (entry[2] == 'd' && entry[3] == 'c') {
        frame_offsets_[kept] = payload;
        frame_sizes_[kept] = size;
        largest_frame_ = size > largest_frame_ ? size : largest_frame_;
        ++kept;
      } else if (wants_audio && entry[2] == 'w' && entry[3] == 'b') {
        audio_offsets_[audio_chunk_count_] = payload;
        audio_sizes_[audio_chunk_count_] = size;
        audio_bytes_ += size;
        largest_audio_chunk_ =
            size > largest_audio_chunk_ ? size : largest_audio_chunk_;
        ++audio_chunk_count_;
      }
    }
    if (got != wanted) {
      break;
    }
  }
  heap_caps_free(block);

  if (kept == 0) {
    Close();
    return false;
  }
  frame_count_ = kept;
  if (audio_chunk_count_ == 0 &&
      microseconds_per_frame_ == kLegacyMicrosecondsPerFrame &&
      recorded_by_this_phone) {
    microseconds_per_frame_ = kLegacyRealMicrosecondsPerFrame;
    frames_per_second_ = 10;
  }
  if (frames_per_second_ == 0) {
    frames_per_second_ = 15;
  }
  return true;
}

void AviReader::Close() {
  if (file_ != nullptr) {
    std::fclose(file_);
    file_ = nullptr;
  }
  heap_caps_free(frame_offsets_);
  heap_caps_free(frame_sizes_);
  heap_caps_free(audio_offsets_);
  heap_caps_free(audio_sizes_);
  frame_offsets_ = nullptr;
  frame_sizes_ = nullptr;
  audio_offsets_ = nullptr;
  audio_sizes_ = nullptr;
  audio_sample_rate_hz_ = 0;
  audio_channel_count_ = 0;
  audio_bits_per_sample_ = 0;
  audio_chunk_count_ = 0;
  largest_audio_chunk_ = 0;
  audio_bytes_ = 0;
  microseconds_per_frame_ = 66666;
  frame_count_ = 0;
  largest_frame_ = 0;
  width_ = 0;
  height_ = 0;
}

void AviReader::ReadAudioFormat(long position, long end) {
  // Inside hdrl: one 'strl' list per stream, each a 'strh' and then a 'strf'.
  while (position + 12 <= end) {
    uint8_t list[12] = {};
    if (!ReadAt(file_, position, list, sizeof(list))) {
      return;
    }
    const uint32_t size = ReadUint32(list + 4);
    if (std::memcmp(list, "LIST", 4) == 0 &&
        std::memcmp(list + 8, "strl", 4) == 0) {
      uint8_t stream[8 + 8] = {};
      if (ReadAt(file_, position + 12, stream, sizeof(stream)) &&
          std::memcmp(stream, "strh", 4) == 0 &&
          std::memcmp(stream + 8, "auds", 4) == 0) {
        const uint32_t header_size = ReadUint32(stream + 4);
        uint8_t format[8 + 16] = {};
        const long format_position = position + 12 + 8 +
            static_cast<long>(header_size) + (header_size & 1);
        if (ReadAt(file_, format_position, format, sizeof(format)) &&
            std::memcmp(format, "strf", 4) == 0 &&
            (format[8] | (format[9] << 8)) == kWaveFormatPcm &&
            format[22] == 16) {
          audio_channel_count_ = format[10];
          audio_sample_rate_hz_ = ReadUint32(format + 12);
          audio_bits_per_sample_ = format[22];
        }
        return;
      }
    }
    position += 8 + static_cast<long>(size) + (size & 1);
  }
}

bool AviReader::FindAudioChunk(
    uint64_t byte_position, uint32_t* chunk, uint64_t* chunk_start) const {
  if (chunk == nullptr || chunk_start == nullptr ||
      byte_position >= audio_bytes_) {
    return false;
  }
  uint64_t start = 0;
  for (uint32_t index = 0; index < audio_chunk_count_; ++index) {
    if (byte_position < start + audio_sizes_[index]) {
      *chunk = index;
      *chunk_start = start;
      return true;
    }
    start += audio_sizes_[index];
  }
  return false;
}

bool AviReader::ReadAudioChunk(
    uint32_t index, uint8_t* buffer, uint32_t capacity, uint32_t* size) {
  if (file_ == nullptr || buffer == nullptr || size == nullptr ||
      index >= audio_chunk_count_ || audio_sizes_[index] > capacity) {
    return false;
  }
  if (!ReadAt(file_, static_cast<long>(audio_offsets_[index]), buffer,
          audio_sizes_[index])) {
    return false;
  }
  *size = audio_sizes_[index];
  return true;
}

bool AviReader::ReadFrame(
    uint32_t index, uint8_t* buffer, uint32_t capacity, uint32_t* size) {
  if (file_ == nullptr || buffer == nullptr || size == nullptr ||
      index >= frame_count_) {
    return false;
  }
  const uint32_t frame_size = frame_sizes_[index];
  if (frame_size > capacity) {
    return false;
  }
  if (!ReadAt(file_, static_cast<long>(frame_offsets_[index]), buffer,
          frame_size)) {
    return false;
  }
  *size = frame_size;
  return true;
}

}  // namespace lilygo_box::survival
