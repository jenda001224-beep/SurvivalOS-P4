/*
 * @Description: Camera preview provider interface
 * @Author: LILYGO_L
 * @Date: 2026-07-02 00:00:00
 * @LastEditTime: 2026-07-02 00:00:00
 * @License: GPL 3.0
 */
#pragma once

#include <cstddef>
#include <cstdint>

#include "app/diagnostics/camera_error.h"

namespace lilygo_box::hal {

struct CameraPreviewFrameInfo {
  size_t data_size = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t bits_per_pixel = 0;
  uint32_t sequence = 0;
};

enum class VideoRecordingState : uint8_t {
  kIdle,
  kRecording,
  kCompleted,
  kError,
};

struct VideoRecordingStatus {
  VideoRecordingState state = VideoRecordingState::kIdle;
  uint32_t elapsed_ms = 0;
  uint32_t frame_count = 0;
  uint64_t bytes_written = 0;
  // Frames actually reaching the file, which drops below the target when the
  // card cannot keep up.
  uint32_t frames_per_second = 0;
};

class CameraProvider {
 public:
  virtual ~CameraProvider() = default;

  /**
   * @brief 启动摄像头预览并更新内部帧缓冲区
   * @return 启动请求成功提交返回 true，否则返回 false
   */
  virtual bool StartCameraPreview() = 0;

  /**
   * @brief 获取最近一次摄像头预览启动错误
   * @return 摄像头错误
   */
  virtual CameraError GetCameraPreviewError() const = 0;

  /**
   * @brief 请求后台停止摄像头预览但不等待任务退出
   */
  virtual void RequestCameraPreviewStop() = 0;

  /**
   * @brief 停止摄像头预览
   * @return 停止成功或已经停止返回 true，否则返回 false
   */
  virtual bool StopCameraPreview() = 0;

  /**
   * @brief 获取最新摄像头预览帧信息
   * @param info 预览帧信息输出地址
   * @return 获取成功返回 true，否则返回 false
   */
  virtual bool GetCameraPreviewFrameInfo(CameraPreviewFrameInfo* info) = 0;

  /**
   * @brief 复制最新摄像头预览帧到调用方缓冲区
   * @param buffer 输出缓冲区
   * @param buffer_size 输出缓冲区大小
   * @param info 预览帧信息输出地址
   * @return 复制成功返回 true，否则返回 false
   */
  virtual bool CopyCameraPreviewFrame(
      uint8_t* buffer, size_t buffer_size, CameraPreviewFrameInfo* info) = 0;

  /**
   * @brief Encodes the current preview frame as a JPEG and writes it to a file.
   * @param path Absolute path of the file to create.
   * @return true when a photo was written.
   */
  virtual bool CaptureCameraPhoto(const char* path) {
    (void)path;
    return false;
  }

  /**
   * @brief Starts recording the preview into a Motion JPEG video.
   * @param path Absolute path of the .avi file to create.
   * @return true when recording started.
   */
  virtual bool StartVideoRecording(const char* path) {
    (void)path;
    return false;
  }

  /**
   * @brief Stops the current recording and finalises the file.
   * @return true when the stop request was accepted.
   */
  virtual bool StopVideoRecording() { return false; }

  /**
   * @brief Reads the state of the current or last recording.
   * @param status Status output.
   * @return true when the status was filled in.
   */
  virtual bool ReadVideoRecordingStatus(VideoRecordingStatus* status) {
    (void)status;
    return false;
  }
};

}  // namespace lilygo_box::hal
