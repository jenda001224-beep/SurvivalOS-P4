/**
 * @Description: Plays the sound track of a video
 * @License: GPL 3.0
 */
#include "audio/avi_audio.h"

#include <algorithm>
#include <memory>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "survival/avi_reader.h"

namespace lilygo_box::audio {
namespace {

// Small pieces, because the progress reported here is the clock the picture
// follows: 4 kB of 44.1 kHz stereo is 23 ms, a third of a frame.
constexpr uint32_t kWritePieceBytes = 4 * 1024;

}  // namespace

PcmPlaybackResult PlayAviAudio(const char* path, PcmOutput* output) {
  if (path == nullptr || output == nullptr) {
    return PcmPlaybackResult::kOpenFailed;
  }
  survival::AviReader reader;
  if (!reader.Open(path)) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Open video failed: %s\n", path);
    return PcmPlaybackResult::kOpenFailed;
  }
  if (!reader.has_audio()) {
    return PcmPlaybackResult::kInvalidStream;
  }
  if (!output->Configure(reader.audio_sample_rate_hz(),
          reader.audio_channel_count(), reader.audio_bits_per_sample())) {
    return PcmPlaybackResult::kOutputFailed;
  }

  std::unique_ptr<uint8_t, void (*)(void*)> buffer(
      static_cast<uint8_t*>(heap_caps_malloc(reader.largest_audio_chunk(),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)),
      heap_caps_free);
  if (buffer == nullptr) {
    return PcmPlaybackResult::kDecoderFailed;
  }

  const uint32_t block =
      reader.audio_channel_count() * (reader.audio_bits_per_sample() / 8U);
  const uint32_t byte_rate = reader.audio_sample_rate_hz() * block;
  uint64_t position = 0;
  uint32_t chunk = 0;
  uint32_t skip = 0;
  while (chunk < reader.audio_chunk_count()) {
    if (!output->WaitUntilReady()) {
      return PcmPlaybackResult::kStopped;
    }

    uint32_t seek_position_ms = 0;
    if (output->TakeSeekRequest(&seek_position_ms)) {
      uint64_t target =
          static_cast<uint64_t>(seek_position_ms) * byte_rate / 1000U;
      // Always land on a frame boundary, otherwise the channels swap over.
      target -= target % block;
      uint64_t chunk_start = 0;
      if (!reader.FindAudioChunk(target, &chunk, &chunk_start)) {
        output->UpdateProgress(static_cast<uint32_t>(
            reader.audio_bytes() * 1000U / byte_rate));
        return PcmPlaybackResult::kCompleted;
      }
      position = target;
      skip = static_cast<uint32_t>(target - chunk_start);
    }

    uint32_t size = 0;
    if (!reader.ReadAudioChunk(chunk, buffer.get(),
            reader.largest_audio_chunk(), &size)) {
      return PcmPlaybackResult::kDecoderFailed;
    }
    for (uint32_t offset = skip; offset < size;) {
      const uint32_t piece = std::min(kWritePieceBytes, size - offset);
      if (!output->Write(buffer.get() + offset, piece)) {
        return output->WaitUntilReady() ? PcmPlaybackResult::kOutputFailed
                                        : PcmPlaybackResult::kStopped;
      }
      offset += piece;
      position += piece;
      output->UpdateProgress(
          static_cast<uint32_t>(position * 1000U / byte_rate));
    }
    skip = 0;
    ++chunk;
  }
  return PcmPlaybackResult::kCompleted;
}

}  // namespace lilygo_box::audio
