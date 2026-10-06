/**
 * @Description: Streaming WAV playback
 * @License: GPL 3.0
 */
#include "audio/wav_decoder.h"

#include <cstdio>
#include <cstring>
#include <memory>

#include "base/logger.h"
#include "esp_heap_caps.h"

namespace lilygo_box::audio {
namespace {

// Big enough that the SD card is read in useful bursts, small enough that a
// stop or seek request is noticed promptly.
constexpr size_t kPlaybackChunkBytes = 8 * 1024;
constexpr uint16_t kWaveFormatPcm = 1;

/**
 * @brief Reads a little-endian 32-bit value.
 * @param data Four bytes.
 * @return The decoded value.
 */
uint32_t ReadUint32(const uint8_t* data) {
  return static_cast<uint32_t>(data[0]) |
      (static_cast<uint32_t>(data[1]) << 8) |
      (static_cast<uint32_t>(data[2]) << 16) |
      (static_cast<uint32_t>(data[3]) << 24);
}

/**
 * @brief Reads a little-endian 16-bit value.
 * @param data Two bytes.
 * @return The decoded value.
 */
uint16_t ReadUint16(const uint8_t* data) {
  return static_cast<uint16_t>(
      static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
}

/**
 * @brief Walks the RIFF chunks and stops at the start of the PCM payload.
 * @param file Open file positioned at the start.
 * @param info Stream description output.
 * @param payload_offset Byte offset of the payload output.
 * @return true when a playable PCM stream was found.
 */
bool ParseHeader(FILE* file, WavStreamInfo* info, long* payload_offset) {
  uint8_t riff[12] = {};
  if (std::fread(riff, 1, sizeof(riff), file) != sizeof(riff) ||
      std::memcmp(riff, "RIFF", 4) != 0 ||
      std::memcmp(riff + 8, "WAVE", 4) != 0) {
    return false;
  }

  bool format_found = false;
  uint16_t block_align = 0;
  while (true) {
    uint8_t header[8] = {};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
      return false;
    }
    const uint32_t chunk_size = ReadUint32(header + 4);

    if (std::memcmp(header, "fmt ", 4) == 0) {
      uint8_t format[16] = {};
      const uint32_t wanted = chunk_size < sizeof(format) ? chunk_size
                                                          : sizeof(format);
      if (std::fread(format, 1, wanted, file) != wanted) {
        return false;
      }
      if (ReadUint16(format) != kWaveFormatPcm) {
        return false;
      }
      info->channel_count = static_cast<uint8_t>(ReadUint16(format + 2));
      info->sample_rate_hz = ReadUint32(format + 4);
      block_align = ReadUint16(format + 12);
      info->bits_per_sample = static_cast<uint8_t>(ReadUint16(format + 14));
      format_found = true;
      // Chunks are word aligned and may carry extra bytes we do not need.
      const long skip = static_cast<long>(chunk_size - wanted) +
          (chunk_size % 2 == 0 ? 0 : 1);
      if (skip > 0 && std::fseek(file, skip, SEEK_CUR) != 0) {
        return false;
      }
      continue;
    }

    if (std::memcmp(header, "data", 4) == 0) {
      if (!format_found || info->channel_count == 0 ||
          info->sample_rate_hz == 0 ||
          (info->bits_per_sample != 16 && info->bits_per_sample != 8)) {
        return false;
      }
      if (block_align == 0) {
        block_align = static_cast<uint16_t>(
            info->channel_count * (info->bits_per_sample / 8));
      }
      info->payload_bytes = chunk_size;
      const uint32_t byte_rate = info->sample_rate_hz * block_align;
      info->duration_ms = byte_rate == 0
          ? 0
          : static_cast<uint32_t>(
                static_cast<uint64_t>(chunk_size) * 1000U / byte_rate);
      *payload_offset = std::ftell(file);
      return *payload_offset >= 0;
    }

    const long skip =
        static_cast<long>(chunk_size) + (chunk_size % 2 == 0 ? 0 : 1);
    if (std::fseek(file, skip, SEEK_CUR) != 0) {
      return false;
    }
  }
}

}  // namespace

bool ReadWavStreamInfo(const char* path, WavStreamInfo* info) {
  if (path == nullptr || info == nullptr) {
    return false;
  }
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  WavStreamInfo parsed;
  long payload_offset = 0;
  const bool ok = ParseHeader(file, &parsed, &payload_offset);
  std::fclose(file);
  if (ok) {
    *info = parsed;
  }
  return ok;
}

PcmPlaybackResult PlayWavFile(const char* path, PcmOutput* output) {
  if (path == nullptr || output == nullptr) {
    return PcmPlaybackResult::kOpenFailed;
  }

  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Open WAV failed: %s\n", path);
    return PcmPlaybackResult::kOpenFailed;
  }

  WavStreamInfo info;
  long payload_offset = 0;
  if (!ParseHeader(file, &info, &payload_offset)) {
    std::fclose(file);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Unsupported WAV stream: %s\n", path);
    return PcmPlaybackResult::kInvalidStream;
  }

  if (!output->Configure(
          info.sample_rate_hz, info.channel_count, info.bits_per_sample)) {
    std::fclose(file);
    return PcmPlaybackResult::kOutputFailed;
  }

  // The transfer buffer is DMA-capable so the I2S write path never has to
  // bounce it through another allocation.
  std::unique_ptr<uint8_t, void (*)(void*)> buffer(
      static_cast<uint8_t*>(
          heap_caps_malloc(kPlaybackChunkBytes, MALLOC_CAP_DMA)),
      heap_caps_free);
  if (buffer == nullptr) {
    std::fclose(file);
    return PcmPlaybackResult::kDecoderFailed;
  }

  const uint16_t block_align = static_cast<uint16_t>(
      info.channel_count * (info.bits_per_sample / 8));
  const uint32_t byte_rate = info.sample_rate_hz * block_align;
  uint32_t consumed_bytes = 0;
  PcmPlaybackResult result = PcmPlaybackResult::kCompleted;

  while (consumed_bytes < info.payload_bytes) {
    if (!output->WaitUntilReady()) {
      result = PcmPlaybackResult::kStopped;
      break;
    }

    uint32_t seek_position_ms = 0;
    if (output->TakeSeekRequest(&seek_position_ms) && byte_rate > 0) {
      uint32_t target = static_cast<uint32_t>(
          static_cast<uint64_t>(seek_position_ms) * byte_rate / 1000U);
      // Always land on a frame boundary, otherwise the channels swap over.
      target -= target % block_align;
      if (target > info.payload_bytes) {
        target = info.payload_bytes;
      }
      if (std::fseek(file, payload_offset + static_cast<long>(target),
              SEEK_SET) != 0) {
        result = PcmPlaybackResult::kDecoderFailed;
        break;
      }
      consumed_bytes = target;
      if (consumed_bytes >= info.payload_bytes) {
        break;
      }
    }

    const size_t wanted = static_cast<size_t>(
        info.payload_bytes - consumed_bytes < kPlaybackChunkBytes
            ? info.payload_bytes - consumed_bytes
            : kPlaybackChunkBytes);
    const size_t read_bytes = std::fread(buffer.get(), 1, wanted, file);
    if (read_bytes == 0) {
      // The data chunk claimed more than the file holds; treat it as the end.
      break;
    }
    if (!output->Write(buffer.get(), read_bytes)) {
      result = PcmPlaybackResult::kOutputFailed;
      break;
    }
    consumed_bytes += static_cast<uint32_t>(read_bytes);
    if (byte_rate > 0) {
      output->UpdateProgress(static_cast<uint32_t>(
          static_cast<uint64_t>(consumed_bytes) * 1000U / byte_rate));
    }
  }

  std::fclose(file);
  return result;
}

}  // namespace lilygo_box::audio
