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
   * @brief How long one frame lasts.
   * @return Microseconds per frame, never zero.
   */
  uint32_t microseconds_per_frame() const { return microseconds_per_frame_; }

  /**
   * @brief Whether the video carries PCM sound this reader can hand out.
   * @return true when it has sound.
   */
  bool has_audio() const { return audio_chunk_count_ > 0; }

  uint32_t audio_sample_rate_hz() const { return audio_sample_rate_hz_; }
  uint8_t audio_channel_count() const { return audio_channel_count_; }
  uint8_t audio_bits_per_sample() const { return audio_bits_per_sample_; }
  uint32_t audio_chunk_count() const { return audio_chunk_count_; }
  uint32_t largest_audio_chunk() const { return largest_audio_chunk_; }

  /**
   * @brief Total size of the sound.
   * @return Size in bytes.
   */
  uint64_t audio_bytes() const { return audio_bytes_; }

  /**
   * @brief Finds the sound chunk holding a position.
   * @param byte_position Position in the sound, in bytes from its start.
   * @param chunk Index of the chunk output.
   * @param chunk_start Position at which that chunk starts output.
   * @return true when the position lies inside the sound.
   */
  bool FindAudioChunk(
      uint64_t byte_position, uint32_t* chunk, uint64_t* chunk_start) const;

  /**
   * @brief Reads one chunk of sound.
   * @param index Chunk index.
   * @param buffer Destination buffer.
   * @param capacity Size of the destination buffer.
   * @param size Number of bytes written.
   * @return true when the chunk was read.
   */
  bool ReadAudioChunk(
      uint32_t index, uint8_t* buffer, uint32_t capacity, uint32_t* size);

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
  /**
   * @brief Looks through the stream headers for a PCM sound stream.
   * @param position Offset of the first stream list inside hdrl.
   * @param end Offset just past hdrl.
   */
  void ReadAudioFormat(long position, long end);

  FILE* file_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t frames_per_second_ = 15;
  uint32_t frame_count_ = 0;
  uint32_t largest_frame_ = 0;
  uint32_t microseconds_per_frame_ = 66666;
  // Absolute file offsets of each frame's payload, plus its size.
  uint32_t* frame_offsets_ = nullptr;
  uint32_t* frame_sizes_ = nullptr;
  uint32_t audio_sample_rate_hz_ = 0;
  uint8_t audio_channel_count_ = 0;
  uint8_t audio_bits_per_sample_ = 0;
  uint32_t audio_chunk_count_ = 0;
  uint32_t largest_audio_chunk_ = 0;
  uint64_t audio_bytes_ = 0;
  // The same for each chunk of sound.
  uint32_t* audio_offsets_ = nullptr;
  uint32_t* audio_sizes_ = nullptr;
};

}  // namespace lilygo_box::survival
