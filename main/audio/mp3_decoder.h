/*
 * @Description: MP3 流式解码与 PCM 输出接口
 * @Author: LILYGO_L
 * @Date: 2026-07-14 22:50:00
 * @LastEditTime: 2026-07-15 11:16:11
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "audio/pcm_output.h"

namespace lilygo_box::audio {

// The PCM sink and the result codes are shared with the other decoders.
using Mp3PlaybackResult = PcmPlaybackResult;

/**
 * @brief 流式解码并播放一个 MP3 文件
 * @param path MP3 文件绝对路径
 * @param output PCM 输出接口
 * @return 播放结束原因
 */
Mp3PlaybackResult PlayMp3File(const char* path, PcmOutput* output);

}  // namespace lilygo_box::audio
