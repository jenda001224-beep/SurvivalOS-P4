/**
 * @Description: gzip and raw deflate decompression
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace lilygo_box::survival {

/**
 * @brief Decompresses a gzip or zlib stream into a freshly allocated buffer.
 *
 * PMTiles stores both its directories and its tiles gzip compressed. The
 * ESP32-P4 ROM ships the miniz inflate routines, so this costs no flash at all.
 *
 * @param input Compressed bytes.
 * @param input_size Number of compressed bytes.
 * @param output Receives a PSRAM buffer the caller frees with
 *        heap_caps_free(); left null on failure.
 * @param output_size Receives the decompressed size.
 * @param size_hint Expected decompressed size, or 0 when unknown.
 * @return true when the stream was decompressed.
 */
bool InflateToBuffer(const uint8_t* input, size_t input_size, uint8_t** output,
    size_t* output_size, size_t size_hint);

}  // namespace lilygo_box::survival
