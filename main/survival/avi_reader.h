/**
 * @Description: Reads Motion JPEG video out of an AVI container
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>
#include <cstdio>

namespace lilygo_box::survival {

// Reads back the files AviWriter produces, and any other Motion JPEG AVI whose
// frames are indexed. Frames are handed out as raw JPEG so the caller can push
// them straight through the hardware decoder.
class AviReader {
 public:
  AviReader() = default;
  ~AviReader();

  AviReader(const AviReader&) = delete;
  AviReader& operator=(const AviReader&) = delete;

  /**
   * @brief Opens a video and reads its header and index.
   * @param path Absolute path of the .avi file.
   * @return true when the file holds playable Motion JPEG.
   */
  bool Open(const char* path);

  /**
   * @brief Closes the file and frees the index.
   */
  void Close();

  /**
   * @brief Reports whether a video is open.
   * @return true when open.
   */
  bool IsOpen() const { return file_ != nullptr; }

  /**
   * @brief Number of frames in the video.
   * @return Frame count.
   */
  uint32_t frame_count() const { return frame_count_; }

  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }

  /**
   * @brief Nominal frame rate.
   * @return Frames per second, never zero.
   */
  uint32_t frames_per_second() const { return frames_per_second_; }

  /**
   * @brief Length of the video.
   * @return Duration in milliseconds.
   */
  uint32_t duration_ms() const;

  /**
   * @brief Reads one frame as JPEG.
   * @param index Frame index.
   * @param buffer Destination buffer.
   * @param capacity Size of the destination buffer.
   * @param size Number of bytes written.
   * @return true when the frame was read.
   */
  bool ReadFrame(
      uint32_t index, uint8_t* buffer, uint32_t capacity, uint32_t* size);

  /**
   * @brief Size of the largest frame, so a caller can size one buffer once.
   * @return Size in bytes.
   */
  uint32_t largest_frame() const { return largest_frame_; }

 private:
  FILE* file_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t frames_per_second_ = 15;
  uint32_t frame_count_ = 0;
  uint32_t largest_frame_ = 0;
  // Absolute file offsets of each frame's payload, plus its size.
  uint32_t* frame_offsets_ = nullptr;
  uint32_t* frame_sizes_ = nullptr;
};

}  // namespace lilygo_box::survival
