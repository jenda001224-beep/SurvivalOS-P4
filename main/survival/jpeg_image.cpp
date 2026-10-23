/**
 * @Description: JPEG file loading for the photo library
 * @License: GPL 3.0
 */
#include "survival/jpeg_image.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "base/logger.h"
#include "driver/jpeg_decode.h"
#include "driver/jpeg_encode.h"
#include "esp_heap_caps.h"
#include "hal/ppa/ppa_srm_helper.h"

namespace lilygo_box::survival {
namespace {

constexpr int kDecodeTimeoutMs = 4000;
// Guards against a corrupt or absurd file eating all of PSRAM.
constexpr size_t kMaxFileBytes = 8 * 1024 * 1024;
constexpr uint32_t kMaxPixels = 4096 * 4096;
// The PPA scaler only accepts factors in 1/16 steps between 1/16 and 16.
constexpr float kMinimumScale = 1.0F / 16.0F;
constexpr float kScaleStep = 1.0F / 16.0F;

/**
 * @brief Reads a whole file into a decoder-friendly buffer.
 * @param path Absolute path of the file.
 * @param size_bytes Size of the loaded data.
 * @return The buffer, or nullptr on failure. Release with free().
 */
uint8_t* ReadFileIntoDecoderBuffer(const char* path, size_t* size_bytes) {
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return nullptr;
  }
  if (std::fseek(file, 0, SEEK_END) != 0) {
    std::fclose(file);
    return nullptr;
  }
  const long length = std::ftell(file);
  if (length <= 0 || static_cast<size_t>(length) > kMaxFileBytes ||
      std::fseek(file, 0, SEEK_SET) != 0) {
    std::fclose(file);
    return nullptr;
  }

  const jpeg_decode_memory_alloc_cfg_t config = {
      .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
  };
  size_t capacity = 0;
  auto* buffer = static_cast<uint8_t*>(
      jpeg_alloc_decoder_mem(static_cast<size_t>(length), &config, &capacity));
  if (buffer == nullptr) {
    std::fclose(file);
    return nullptr;
  }
  const size_t read_bytes =
      std::fread(buffer, 1, static_cast<size_t>(length), file);
  std::fclose(file);
  if (read_bytes != static_cast<size_t>(length)) {
    free(buffer);
    return nullptr;
  }
  *size_bytes = read_bytes;
  return buffer;
}

/**
 * @brief Rounds a scale factor down onto the PPA's 1/16 grid.
 * @param scale Desired factor.
 * @return A factor the PPA accepts, never below 1/16.
 */
float QuantizeScale(float scale) {
  if (scale >= 1.0F) {
    return 1.0F;
  }
  const float steps = std::max(1.0F, std::floor(scale / kScaleStep));
  return std::max(kMinimumScale, steps * kScaleStep);
}

}  // namespace

bool ReadJpegSize(const char* path, uint32_t* width, uint32_t* height) {
  if (path == nullptr || width == nullptr || height == nullptr) {
    return false;
  }
  size_t size_bytes = 0;
  uint8_t* data = ReadFileIntoDecoderBuffer(path, &size_bytes);
  if (data == nullptr) {
    return false;
  }
  jpeg_decode_picture_info_t info = {};
  const esp_err_t error = jpeg_decoder_get_info(
      data, static_cast<uint32_t>(size_bytes), &info);
  free(data);
  if (error != ESP_OK) {
    return false;
  }
  *width = info.width;
  *height = info.height;
  return true;
}

bool LoadJpegImage(const char* path, uint32_t max_width, uint32_t max_height,
    JpegImage* image) {
  if (path == nullptr || image == nullptr) {
    return false;
  }
  size_t file_bytes = 0;
  uint8_t* file_data = ReadFileIntoDecoderBuffer(path, &file_bytes);
  if (file_data == nullptr) {
    return false;
  }
  const bool decoded =
      DecodeJpegBuffer(file_data, file_bytes, max_width, max_height, image);
  free(file_data);
  return decoded;
}

bool DecodeJpegBuffer(const uint8_t* data, size_t size, uint32_t max_width,
    uint32_t max_height, JpegImage* image) {
  if (data == nullptr || size == 0 || image == nullptr) {
    return false;
  }
  *image = JpegImage();

  // The hardware decoder reads its input over the cache, so the bytes have to
  // sit in a buffer it allocated. A caller's buffer is copied in rather than
  // making every caller know that.
  const jpeg_decode_memory_alloc_cfg_t input_config = {
      .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
  };
  size_t input_capacity = 0;
  auto* owned = static_cast<uint8_t*>(
      jpeg_alloc_decoder_mem(size, &input_config, &input_capacity));
  if (owned == nullptr) {
    return false;
  }
  std::memcpy(owned, data, size);
  const uint8_t* file_data = owned;
  const size_t file_bytes = size;

  jpeg_decode_picture_info_t info = {};
  if (jpeg_decoder_get_info(file_data, static_cast<uint32_t>(file_bytes),
          &info) != ESP_OK ||
      info.width == 0 || info.height == 0 ||
      static_cast<uint32_t>(info.width) * info.height > kMaxPixels) {
    free(owned);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Unreadable JPEG (%u bytes)\n", static_cast<unsigned int>(size));
    return false;
  }

  // The hardware decoder always produces full resolution; the PPA does the
  // scaling afterwards.
  const size_t full_bytes =
      static_cast<size_t>(info.width) * info.height * sizeof(uint16_t);
  const jpeg_decode_memory_alloc_cfg_t output_config = {
      .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
  };
  size_t full_capacity = 0;
  auto* full_pixels = static_cast<uint8_t*>(
      jpeg_alloc_decoder_mem(full_bytes, &output_config, &full_capacity));
  if (full_pixels == nullptr) {
    free(owned);
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Allocate %u bytes for JPEG output failed\n",
        static_cast<unsigned int>(full_bytes));
    return false;
  }

  jpeg_decoder_handle_t decoder = nullptr;
  const jpeg_decode_engine_cfg_t engine_config = {
      .intr_priority = 0,
      .timeout_ms = kDecodeTimeoutMs,
  };
  if (jpeg_new_decoder_engine(&engine_config, &decoder) != ESP_OK) {
    free(owned);
    free(full_pixels);
    return false;
  }

  // LVGL's RGB565 is little endian. The driver's "RGB" order swaps the two
  // bytes of every pixel (big endian), which turned every photo into garbage
  // colours; "BGR" leaves them in native order, as the IDF LCD example does.
  const jpeg_decode_cfg_t decode_config = {
      .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
      .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
      .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
  };
  uint32_t decoded_bytes = 0;
  const esp_err_t error = jpeg_decoder_process(decoder, &decode_config,
      file_data, static_cast<uint32_t>(file_bytes), full_pixels,
      static_cast<uint32_t>(full_capacity), &decoded_bytes);
  jpeg_del_decoder_engine(decoder);
  free(owned);

  if (error != ESP_OK || decoded_bytes == 0) {
    free(full_pixels);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "JPEG decode failed: %s\n", esp_err_to_name(error));
    return false;
  }

  image->source_width = info.width;
  image->source_height = info.height;

  const float scale_x = max_width == 0
      ? 1.0F
      : static_cast<float>(max_width) / static_cast<float>(info.width);
  const float scale_y = max_height == 0
      ? 1.0F
      : static_cast<float>(max_height) / static_cast<float>(info.height);
  const float scale = QuantizeScale(std::min(scale_x, scale_y));
  if (scale >= 1.0F) {
    image->pixels = full_pixels;
    image->size_bytes = full_capacity;
    image->width = info.width;
    image->height = info.height;
    return true;
  }

  const uint32_t target_width = std::max<uint32_t>(1,
      static_cast<uint32_t>(static_cast<float>(info.width) * scale));
  const uint32_t target_height = std::max<uint32_t>(1,
      static_cast<uint32_t>(static_cast<float>(info.height) * scale));

  hal::PpaSrmHelper ppa;
  if (!ppa.Init()) {
    // Without the accelerator the full-size image is still usable, just large.
    image->pixels = full_pixels;
    image->size_bytes = full_capacity;
    image->width = info.width;
    image->height = info.height;
    return true;
  }

  const size_t alignment = std::max<size_t>(64, ppa.CacheLineSize());
  const size_t target_bytes = hal::AlignUp(
      static_cast<size_t>(target_width) * target_height * sizeof(uint16_t),
      alignment);
  auto* scaled_pixels = static_cast<uint8_t*>(heap_caps_aligned_alloc(
      alignment, target_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (scaled_pixels == nullptr) {
    image->pixels = full_pixels;
    image->size_bytes = full_capacity;
    image->width = info.width;
    image->height = info.height;
    return true;
  }

  hal::PpaSrmImageConfig input;
  input.buffer = full_pixels;
  input.buffer_size = full_capacity;
  input.pic_width = info.width;
  input.pic_height = info.height;
  input.block_width = info.width;
  input.block_height = info.height;

  hal::PpaSrmImageConfig output;
  output.buffer = scaled_pixels;
  output.buffer_size = target_bytes;
  output.pic_width = target_width;
  output.pic_height = target_height;
  output.block_width = target_width;
  output.block_height = target_height;

  hal::PpaSrmTransformConfig transform;
  transform.scale_x = scale;
  transform.scale_y = scale;

  if (!ppa.Transform(input, output, transform)) {
    heap_caps_free(scaled_pixels);
    image->pixels = full_pixels;
    image->size_bytes = full_capacity;
    image->width = info.width;
    image->height = info.height;
    return true;
  }

  free(full_pixels);
  image->pixels = scaled_pixels;
  image->size_bytes = target_bytes;
  image->width = target_width;
  image->height = target_height;
  return true;
}

bool RunJpegColorSelfTest(char* report, size_t report_size) {
  if (report == nullptr || report_size == 0) {
    return false;
  }
  report[0] = '\0';
  // Four 16-pixel bars in LVGL's native RGB565: red, green, blue, white. They
  // go through the same encoder settings as a photo and back through the
  // viewer's decoder, so a byte-order mistake on either side shows up as a
  // wrong colour here.
  constexpr uint32_t kWidth = 64;
  constexpr uint32_t kHeight = 16;
  constexpr uint16_t kBars[] = {0xF800, 0x07E0, 0x001F, 0xFFFF};
  const size_t raw_bytes = kWidth * kHeight * sizeof(uint16_t);

  const jpeg_encode_memory_alloc_cfg_t input_config = {
      .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
  };
  size_t input_capacity = 0;
  auto* input = static_cast<uint16_t*>(
      jpeg_alloc_encoder_mem(raw_bytes, &input_config, &input_capacity));
  const jpeg_encode_memory_alloc_cfg_t output_config = {
      .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
  };
  size_t output_capacity = 0;
  auto* output = static_cast<uint8_t*>(
      jpeg_alloc_encoder_mem(raw_bytes + 4096, &output_config,
          &output_capacity));
  jpeg_encoder_handle_t encoder = nullptr;
  const jpeg_encode_engine_cfg_t engine_config = {
      .intr_priority = 0,
      .timeout_ms = kDecodeTimeoutMs,
  };
  if (input == nullptr || output == nullptr ||
      jpeg_new_encoder_engine(&engine_config, &encoder) != ESP_OK) {
    free(input);
    free(output);
    std::snprintf(report, report_size, "encoder setup failed");
    return false;
  }
  for (uint32_t y = 0; y < kHeight; ++y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      input[y * kWidth + x] = kBars[x / 16];
    }
  }
  const jpeg_encode_cfg_t encode_config = {
      .height = kHeight,
      .width = kWidth,
      .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
      .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
      .image_quality = 95,
      .pixel_reverse = false,
  };
  uint32_t encoded_size = 0;
  const esp_err_t error = jpeg_encoder_process(encoder, &encode_config,
      reinterpret_cast<uint8_t*>(input), static_cast<uint32_t>(raw_bytes),
      output, static_cast<uint32_t>(output_capacity), &encoded_size);
  jpeg_del_encoder_engine(encoder);
  free(input);
  if (error != ESP_OK || encoded_size == 0) {
    free(output);
    std::snprintf(report, report_size, "encode failed: %s",
        esp_err_to_name(error));
    return false;
  }

  JpegImage image;
  const bool decoded = DecodeJpegBuffer(output, encoded_size, 0, 0, &image);
  free(output);
  if (!decoded || image.width < kWidth || image.height < kHeight) {
    FreeJpegImage(&image);
    std::snprintf(report, report_size, "decode failed");
    return false;
  }

  bool all_close = true;
  size_t used = 0;
  const auto* pixels = reinterpret_cast<const uint16_t*>(image.pixels);
  for (size_t bar = 0; bar < 4; ++bar) {
    const uint16_t got = pixels[8 * image.width + bar * 16 + 8];
    const uint16_t want = kBars[bar];
    // Compression shifts each channel a little; a swap moves it completely.
    const int dr = std::abs(((got >> 11) & 0x1F) - ((want >> 11) & 0x1F));
    const int dg = std::abs(((got >> 5) & 0x3F) - ((want >> 5) & 0x3F));
    const int db = std::abs((got & 0x1F) - (want & 0x1F));
    const bool close = dr <= 3 && dg <= 6 && db <= 3;
    all_close = all_close && close;
    used += static_cast<size_t>(std::snprintf(report + used,
        used < report_size ? report_size - used : 0, "%s%04X->%04X%s",
        bar == 0 ? "" : " ", want, got, close ? "" : "!"));
    if (used >= report_size) {
      break;
    }
  }
  FreeJpegImage(&image);
  return all_close;
}

void FreeJpegImage(JpegImage* image) {
  if (image == nullptr || image->pixels == nullptr) {
    return;
  }
  heap_caps_free(image->pixels);
  *image = JpegImage();
}

}  // namespace lilygo_box::survival
