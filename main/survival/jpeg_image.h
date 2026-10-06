/**
 * @Description: JPEG file loading for the photo library
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

namespace lilygo_box::survival {

// A decoded RGB565 image living in PSRAM. Ownership passes to the caller,
// which releases it with FreeJpegImage().
struct JpegImage {
  uint8_t* pixels = nullptr;
  size_t size_bytes = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // Dimensions of the original file, before any scaling.
  uint32_t source_width = 0;
  uint32_t source_height = 0;
};

/**
 * @brief Reads only the dimensions of a JPEG file.
 * @param path Absolute path of the file.
 * @param width Width output.
 * @param height Height output.
 * @return true when the header was read.
 */
bool ReadJpegSize(const char* path, uint32_t* width, uint32_t* height);

/**
 * @brief Decodes a JPEG file to RGB565, scaled to fit a box.
 *
 * Decoding runs on the ESP32-P4 JPEG accelerator and the scaling on the PPA, so
 * this is fast enough to build a whole thumbnail grid on the fly.
 *
 * @param path Absolute path of the file.
 * @param max_width Widest acceptable result, or 0 for no limit.
 * @param max_height Tallest acceptable result, or 0 for no limit.
 * @param image Decoded image output.
 * @return true when the image was decoded.
 */
bool LoadJpegImage(const char* path, uint32_t max_width, uint32_t max_height,
    JpegImage* image);

/**
 * @brief Decodes JPEG bytes already in memory, scaled to fit a box.
 *
 * Video frames arrive as buffers rather than files, and they go through the
 * same hardware decoder and scaler as photos.
 *
 * @param data JPEG bytes.
 * @param size Number of bytes.
 * @param max_width Widest acceptable result, or 0 for no limit.
 * @param max_height Tallest acceptable result, or 0 for no limit.
 * @param image Decoded image output.
 * @return true when the image was decoded.
 */
bool DecodeJpegBuffer(const uint8_t* data, size_t size, uint32_t max_width,
    uint32_t max_height, JpegImage* image);

/**
 * @brief Releases an image returned by LoadJpegImage().
 * @param image Image to free; safe to call on an empty image.
 */
void FreeJpegImage(JpegImage* image);

}  // namespace lilygo_box::survival
