/**
 * @Description: Streaming WAV playback
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>

#include "audio/pcm_output.h"

namespace lilygo_box::audio {

// What a WAV file turned out to contain. Filled in even when playback is
// stopped early, so callers can show a duration without decoding twice.
struct WavStreamInfo {
  uint32_t sample_rate_hz = 0;
  uint8_t channel_count = 0;
  uint8_t bits_per_sample = 0;
  uint32_t duration_ms = 0;
  uint32_t payload_bytes = 0;
};

/**
 * @brief Reads a WAV file's format without playing it.
 * @param path Absolute path of the file.
 * @param info Stream description output.
 * @return true when the file is a readable 8- or 16-bit PCM WAV.
 */
bool ReadWavStreamInfo(const char* path, WavStreamInfo* info);

/**
 * @brief Streams a WAV file to a PCM sink.
 * @param path Absolute path of the file.
 * @param output PCM sink.
 * @return Why playback ended.
 */
PcmPlaybackResult PlayWavFile(const char* path, PcmOutput* output);

}  // namespace lilygo_box::audio
