/**
 * @Description: Plays the sound track of a video
 * @License: GPL 3.0
 */
#pragma once

#include "audio/pcm_output.h"

namespace lilygo_box::audio {

/**
 * @brief Streams the PCM sound of a Motion JPEG AVI to a PCM sink.
 * @param path Absolute path of the video.
 * @param output PCM sink.
 * @return Why playback ended.
 */
PcmPlaybackResult PlayAviAudio(const char* path, PcmOutput* output);

}  // namespace lilygo_box::audio
