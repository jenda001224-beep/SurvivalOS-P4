/**
 * @Description: gzip and raw deflate decompression
 * @License: GPL 3.0
 */
#include "survival/inflate.h"

#include <cstring>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "miniz.h"

namespace lilygo_box::survival {
namespace {

// A gzip member starts with 0x1F 0x8B and method 8 (deflate).
constexpr uint8_t kGzipMagic0 = 0x1F;
constexpr uint8_t kGzipMagic1 = 0x8B;
constexpr uint8_t kGzipDeflate = 8;
constexpr uint8_t kGzipFlagText = 0x01;
constexpr uint8_t kGzipFlagHeaderCrc = 0x02;
constexpr uint8_t kGzipFlagExtra = 0x04;
constexpr uint8_t kGzipFlagName = 0x08;
constexpr uint8_t kGzipFlagComment = 0x10;

// A map tile or directory that inflates past this is not something this device
// was going to render anyway.
constexpr size_t kMaxOutputBytes = 8 * 1024 * 1024;
constexpr size_t kMinimumGuess = 16 * 1024;

/**
 * @brief Skips a gzip header and reports where the deflate stream starts.
 * @param input Compressed bytes.
 * @param input_size Number of compressed bytes.
 * @param payload_offset Offset of the deflate stream.
 * @param declared_size Size recorded in the gzip trailer, or 0.
 * @return true when the input is a gzip member.
 */
bool ParseGzipHeader(const uint8_t* input, size_t input_size,
    size_t* payload_offset, size_t* declared_size) {
  if (input_size < 18 || input[0] != kGzipMagic0 || input[1] != kGzipMagic1 ||
      input[2] != kGzipDeflate) {
    return false;
  }
  const uint8_t flags = input[3];
  size_t cursor = 10;

  if ((flags & kGzipFlagExtra) != 0) {
    if (cursor + 2 > input_size) {
      return false;
    }
    const size_t extra = static_cast<size_t>(input[cursor]) |
        (static_cast<size_t>(input[cursor + 1]) << 8);
    cursor += 2 + extra;
  }
  if ((flags & kGzipFlagName) != 0) {
    while (cursor < input_size && input[cursor] != 0) {
      ++cursor;
    }
    ++cursor;
  }
  if ((flags & kGzipFlagComment) != 0) {
    while (cursor < input_size && input[cursor] != 0) {
      ++cursor;
    }
    ++cursor;
  }
  if ((flags & kGzipFlagHeaderCrc) != 0) {
    cursor += 2;
  }
  if (cursor + 8 > input_size) {
    return false;
  }
  (void)kGzipFlagText;

  // The last four bytes carry the uncompressed size modulo 2^32, which is an
  // exact allocation hint for everything this reader handles.
  const uint8_t* trailer = input + input_size - 4;
  *declared_size = static_cast<size_t>(trailer[0]) |
      (static_cast<size_t>(trailer[1]) << 8) |
      (static_cast<size_t>(trailer[2]) << 16) |
      (static_cast<size_t>(trailer[3]) << 24);
  *payload_offset = cursor;
  return true;
}

}  // namespace

bool InflateToBuffer(const uint8_t* input, size_t input_size, uint8_t** output,
    size_t* output_size, size_t size_hint) {
  if (input == nullptr || input_size == 0 || output == nullptr ||
      output_size == nullptr) {
    return false;
  }
  *output = nullptr;
  *output_size = 0;

  size_t payload_offset = 0;
  size_t declared_size = 0;
  int flags = 0;
  if (ParseGzipHeader(input, input_size, &payload_offset, &declared_size)) {
    // gzip wraps raw deflate, and the header has already been stepped over.
    flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
  } else if (input_size > 2 && (input[0] & 0x0F) == 8 &&
      ((static_cast<uint32_t>(input[0]) << 8) | input[1]) % 31 == 0) {
    // A zlib wrapper: let miniz parse it.
    flags = TINFL_FLAG_PARSE_ZLIB_HEADER |
        TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
  } else {
    flags = TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF;
  }

  size_t capacity = declared_size;
  if (capacity == 0) {
    capacity = size_hint;
  }
  if (capacity == 0) {
    capacity = input_size * 4;
  }
  if (capacity < kMinimumGuess) {
    capacity = kMinimumGuess;
  }

  while (capacity <= kMaxOutputBytes) {
    auto* buffer = static_cast<uint8_t*>(
        heap_caps_malloc(capacity + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
      return false;
    }
    const size_t produced = tinfl_decompress_mem_to_mem(buffer, capacity,
        input + payload_offset, input_size - payload_offset, flags);
    if (produced != TINFL_DECOMPRESS_MEM_TO_MEM_FAILED) {
      // Callers treat directories and tiles as byte ranges, but terminating the
      // buffer makes it safe to hand to text code too.
      buffer[produced] = 0;
      *output = buffer;
      *output_size = produced;
      return true;
    }
    heap_caps_free(buffer);

    // The only recoverable failure is an output buffer that was too small, and
    // a declared size is exact, so there is nothing to retry in that case.
    if (declared_size != 0) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Inflate failed for a %u byte stream\n",
          static_cast<unsigned int>(input_size));
      return false;
    }
    capacity *= 2;
  }

  LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
      "Inflate output exceeded %u bytes\n",
      static_cast<unsigned int>(kMaxOutputBytes));
  return false;
}

}  // namespace lilygo_box::survival
