/**
 * @Description: Writes Motion JPEG video into an AVI container
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>
#include <cstdio>

namespace lilygo_box::survival {

// Streams JPEG frames into an AVI file.
//
// The ESP32-P4 has a hardware JPEG codec but only a software H.264 decoder, so
// Motion JPEG is what lets the device both record and play its own video at
// full speed. AVI is used rather than MP4 because its header can be patched in
// place at the end, which means recording never needs a second pass over a file
// that may be gigabytes long.
class AviWriter {
 public:
  AviWriter() = default;
  ~AviWriter();

  AviWriter(const AviWriter&) = delete;
  AviWriter& operator=(const AviWriter&) = delete;

  /**
   * @brief Creates a file and reserves space for the header.
   * @param path Absolute path of the file to create.
   * @param width Frame width in pixels.
   * @param height Frame height in pixels.
   * @param frames_per_second Nominal frame rate.
   * @return true when the file was created.
   */
  bool Open(const char* path, uint32_t width, uint32_t height,
      uint32_t frames_per_second);

  /**
   * @brief Appends one encoded JPEG frame.
   * @param data JPEG bytes.
   * @param size Number of bytes.
   * @return true when the frame was written.
   */
  bool AddFrame(const uint8_t* data, uint32_t size);

  /**
   * @brief Writes the index, patches the header and closes the file.
   * @return true when the file was finished cleanly.
   */
  bool Close();

  /**
   * @brief Reports whether a file is open for writing.
   * @return true when open.
   */
  bool IsOpen() const { return file_ != nullptr; }

  /**
   * @brief Number of frames written so far.
   * @return Frame count.
   */
  uint32_t frame_count() const { return frame_count_; }

  /**
   * @brief Bytes written so far, including container overhead.
   * @return Byte count.
   */
  uint64_t bytes_written() const { return bytes_written_; }

  /**
   * @brief Nominal duration of what has been recorded.
   * @return Duration in milliseconds.
   */
  uint32_t duration_ms() const;

 private:
  /**
   * @brief Writes the fixed header block, with sizes that are patched later.
   * @return true when the header was written.
   */
  bool WriteHeader();

  /**
   * @brief Writes the frame index that lets players seek.
   * @return true when the index was written.
   */
  bool WriteIndex();

  FILE* file_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t frames_per_second_ = 15;
  uint32_t frame_count_ = 0;
  uint32_t largest_frame_ = 0;
  uint64_t bytes_written_ = 0;
  uint64_t movi_start_ = 0;

  // The index is built in memory and written at the end. Each entry is 16
  // bytes, so an hour at 15 fps costs about 860 kB.
  uint32_t* index_offsets_ = nullptr;
  uint32_t* index_sizes_ = nullptr;
  uint32_t index_capacity_ = 0;
};

}  // namespace lilygo_box::survival
