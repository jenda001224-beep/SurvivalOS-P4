/*
 * @Description: T-Display-P4 摄像头预览硬件实现
 * @Author: LILYGO_L
 * @Date: 2026-08-28 00:00:00
 * @LastEditTime: 2026-09-02 17:52:54
 * @License: GPL 3.0
 */
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "app/diagnostics/camera_error.h"
#include "app/storage/display_storage.h"
#include "base/logger.h"
#include "driver/jpeg_encode.h"
#include "survival/avi_writer.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "esp_video_ioctl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/device/common/camera_utils.h"
#include "hal/device/t_display_p4/device.h"
#include "linux/videodev2.h"

namespace lilygo_box::hal {
namespace device = lilygo_device_driver::t_display_p4::device;
namespace {

constexpr uint32_t kCameraPreviewTaskStackBytes =
    camera_utils::kPreviewTaskStackBytes;
constexpr UBaseType_t kCameraPreviewTaskPriority =
    camera_utils::kPreviewTaskPriority;
constexpr uint32_t kCameraBufferCount = camera_utils::kBufferCount;
constexpr uint32_t kCameraFrameIntervalMs = camera_utils::kFrameIntervalMs;
constexpr uint32_t kCameraStopWaitTimeoutMs = camera_utils::kStopWaitTimeoutMs;
constexpr uint32_t kCameraSensorReadyPollIntervalMs =
    camera_utils::kSensorReadyPollIntervalMs;
constexpr uint32_t kCameraStartupTimeoutMs = camera_utils::kStartupTimeoutMs;
// Photo capture: quality 90 keeps detail without ballooning the file, and the
// hardware encoder finishes a 1280x720 frame far inside this timeout.
constexpr uint32_t kCaptureJpegQuality = 90;
constexpr int kCaptureEncodeTimeoutMs = 2000;
// Video trades some quality for a frame rate the card can absorb: at 720p and
// quality 80 a frame is roughly 40 kB, so 15 fps is about 600 kB/s.
constexpr uint32_t kVideoJpegQuality = 80;
constexpr uint32_t kVideoTargetFps = 15;
constexpr uint32_t kVideoFramePeriodMs = 1000 / kVideoTargetFps;
constexpr uint32_t kVideoTaskStackBytes = 6 * 1024;
constexpr UBaseType_t kVideoTaskPriority = 4;
constexpr uint32_t kCameraPowerCycleOffDelayMs =
    camera_utils::kPowerCycleOffDelayMs;
constexpr uint32_t kCameraOutputClearFrameCount =
    camera_utils::kOutputClearFrameCount;
constexpr uint32_t kCameraWarmupFrameCount = camera_utils::kWarmupFrameCount;
constexpr uint32_t kCameraVideoInitFlags =
    ESP_VIDEO_INIT_FLAGS_MIPI_CSI | ESP_VIDEO_INIT_FLAGS_ISP;
static_assert(kCameraStartupTimeoutMs >= kCameraSensorReadyPollIntervalMs);
#if defined(CONFIG_LILYGO_DEVICE_DRIVER_CAMERA_TYPE_SC2336)
constexpr uint16_t kCameraSensorI2cAddress = 0x30;
#elif defined(CONFIG_LILYGO_DEVICE_DRIVER_CAMERA_TYPE_OV2710)
constexpr uint16_t kCameraSensorI2cAddress = 0x36;
#elif defined(CONFIG_LILYGO_DEVICE_DRIVER_CAMERA_TYPE_OV5645)
constexpr uint16_t kCameraSensorI2cAddress = 0x3C;
#else
#error "Unsupported camera sensor type"
#endif
constexpr const char* kCameraDeviceName = ESP_VIDEO_MIPI_CSI_DEVICE_NAME;

}  // namespace

void TDisplayP4Device::HeapCapsBufferDeleter::operator()(
    uint8_t* pointer) const {
  if (pointer != nullptr) {
    heap_caps_free(pointer);
  }
}

bool TDisplayP4Device::StartCameraPreview() {
  if (camera_preview_.task_active.load() || camera_preview_.running.load() ||
      camera_preview_.initialized.load()) {
    return !camera_preview_.stop_requested.load();
  }

  camera_preview_.error.store(CameraError::kNone);
  camera_preview_.startup_in_progress.store(false);
  camera_preview_.stop_requested.store(false);
  camera_preview_.task_active.store(true);
  const BaseType_t result = xTaskCreate(CameraPreviewTaskEntry,
      "camera_preview", kCameraPreviewTaskStackBytes, this,
      kCameraPreviewTaskPriority, nullptr);
  if (result != pdPASS) {
    camera_preview_.error.store(CameraError::kPreviewTaskCreateFailed);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Create camera preview task failed: %ld\n", static_cast<long>(result));
    camera_preview_.task_active.store(false);
    camera_preview_.stop_requested.store(true);
    DeinitializeCameraPreview();
    return false;
  }
  return true;
}

CameraError TDisplayP4Device::GetCameraPreviewError() const {
  if (camera_preview_.startup_in_progress.load()) {
    return CameraError::kNone;
  }
  return camera_preview_.error.load();
}

void TDisplayP4Device::RequestCameraPreviewStop() {
  camera_preview_.stop_requested.store(true);
}

bool TDisplayP4Device::StopCameraPreview() {
  RequestCameraPreviewStop();
  // 不在这里发 VIDIOC_STREAMOFF — 让 RunCameraPreviewTask 退出时由
  // DeinitializeCameraPreview 统一处理，避免与正在运行的 DQBUF/PPA 产生 I2C
  // 竞态
  const uint32_t start_ms =
      static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
  while (camera_preview_.task_active.load()) {
    if (static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS) -
            start_ms >=
        kCameraStopWaitTimeoutMs) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "StopCameraPreview timed out\n");
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (camera_preview_.initialized.load()) {
    DeinitializeCameraPreview();
  }
  return true;
}

bool TDisplayP4Device::GetCameraPreviewFrameInfo(CameraPreviewFrameInfo* info) {
  if (info == nullptr || camera_preview_.output_mutex == nullptr ||
      camera_preview_.frame_sequence.load() == 0) {
    return false;
  }
  // SurvivalOS: the geometry changes with the zoom, so it is read under the
  // same lock the preview task holds while switching it.
  if (xSemaphoreTake(camera_preview_.output_mutex, pdMS_TO_TICKS(20)) !=
      pdTRUE) {
    return false;
  }
  const bool ready = camera_preview_.output_buffer != nullptr;
  info->data_size = camera_preview_.output_frame_size;
  info->width = camera_preview_.output_width;
  info->height = camera_preview_.output_height;
  info->stride = camera_preview_.output_stride;
  info->bits_per_pixel = ScreenBitsPerPixel();
  info->sequence = camera_preview_.frame_sequence.load();
  xSemaphoreGive(camera_preview_.output_mutex);
  return ready;
}

bool TDisplayP4Device::CopyCameraPreviewFrame(
    uint8_t* buffer, size_t buffer_size, CameraPreviewFrameInfo* info) {
  if (buffer == nullptr || info == nullptr ||
      camera_preview_.output_mutex == nullptr ||
      camera_preview_.frame_sequence.load() == 0) {
    return false;
  }

  if (xSemaphoreTake(camera_preview_.output_mutex, pdMS_TO_TICKS(20)) !=
      pdTRUE) {
    return false;
  }
  const bool copied = camera_preview_.output_buffer != nullptr &&
                      buffer_size >= camera_preview_.output_frame_size;
  if (copied) {
    info->data_size = camera_preview_.output_frame_size;
    info->width = camera_preview_.output_width;
    info->height = camera_preview_.output_height;
    info->stride = camera_preview_.output_stride;
    info->bits_per_pixel = ScreenBitsPerPixel();
    std::memcpy(buffer, camera_preview_.output_buffer.get(), info->data_size);
    info->sequence = camera_preview_.frame_sequence.load();
  }
  xSemaphoreGive(camera_preview_.output_mutex);
  return copied;
}

void TDisplayP4Device::CameraPreviewTaskEntry(void* context) {
  static_cast<TDisplayP4Device*>(context)->RunCameraPreviewTask();
}

void TDisplayP4Device::RunCameraPreviewTask() {
  if (camera_preview_.stop_requested.load() || !InitializeCameraPreview() ||
      camera_preview_.stop_requested.load()) {
    DeinitializeCameraPreview();
    camera_preview_.stop_requested.store(true);
    camera_preview_.running.store(false);
    camera_preview_.task_active.store(false);
    vTaskDelete(nullptr);
    return;
  }

  camera_preview_.running.store(true);
  while (!camera_preview_.stop_requested.load()) {
    v4l2_buffer buffer = {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (ioctl(camera_preview_.video_fd, VIDIOC_DQBUF, &buffer) != 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    // SurvivalOS: do not try to dequeue a second, newer frame here. DQBUF
    // blocks until a frame is done (the driver ignores O_NONBLOCK), and with
    // two buffers the driver cannot finish the other one while this task
    // still holds the first: both sides would wait for each other forever.

    const bool frame_valid = buffer.index < kCameraBufferCount &&
                             buffer.bytesused > 0 &&
                             (buffer.flags & V4L2_BUF_FLAG_DONE) != 0 &&
                             (buffer.flags & V4L2_BUF_FLAG_ERROR) == 0;
    if (static_cast<uint8_t>(camera_utils::GetPreviewZoom()) !=
        camera_preview_.applied_zoom) {
      ApplyCameraPreviewGeometry();
    }
    if (frame_valid) {
      if (camera_preview_.warmup_frames_remaining > 0) {
        // 传感器刚上电时丢弃少量预热帧，避免未稳定像素短暂显示。
        --camera_preview_.warmup_frames_remaining;
      } else {
        RenderCameraFrame(
            static_cast<uint8_t*>(camera_preview_.frame_buffers[buffer.index]),
            camera_preview_.frame_width, camera_preview_.frame_height);
      }
    }
    ioctl(camera_preview_.video_fd, VIDIOC_QBUF, &buffer);
    vTaskDelay(pdMS_TO_TICKS(kCameraFrameIntervalMs));
  }

  DeinitializeCameraPreview();
  camera_preview_.running.store(false);
  camera_preview_.task_active.store(false);
  vTaskDelete(nullptr);
}

bool TDisplayP4Device::WaitForCameraSensorReady(TickType_t startup_tick) {
  const auto& i2c_bus = driver_.bus().sgm38121_i2c_bus;
  if (i2c_bus == nullptr || i2c_bus->bus_handle() == nullptr) {
    camera_preview_.error.store(CameraError::kSensorNotDetected);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Camera sensor readiness check failed (I2C bus unavailable)\n");
    return false;
  }

  uint32_t attempt = 0;
  while (!camera_utils::StartupTimedOut(startup_tick)) {
    if (camera_preview_.stop_requested.load()) {
      return false;
    }

    ++attempt;
    const TickType_t probe_tick = xTaskGetTickCount();
    const uint32_t remaining_ms =
        camera_utils::StartupRemainingMs(startup_tick);
    const uint32_t probe_timeout_ms =
        std::min(kCameraSensorReadyPollIntervalMs, remaining_ms);
    const esp_err_t result = i2c_master_probe(
        i2c_bus->bus_handle(), kCameraSensorI2cAddress, probe_timeout_ms);
    if (result == ESP_OK) {
      LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
          "Camera sensor ready (address: %#X, attempts: %lu, elapsed: %lu "
          "ms)\n",
          kCameraSensorI2cAddress, static_cast<unsigned long>(attempt),
          static_cast<unsigned long>(
              camera_utils::StartupElapsedMs(startup_tick)));
      return true;
    }
    if (result != ESP_ERR_NOT_FOUND && result != ESP_ERR_TIMEOUT) {
      camera_preview_.error.store(CameraError::kSensorNotDetected);
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Camera sensor readiness check failed (address: %#X, reason: %s, "
          "error: %#X)\n",
          kCameraSensorI2cAddress, esp_err_to_name(result),
          static_cast<unsigned>(result));
      return false;
    }

    const uint32_t probe_elapsed_ms = static_cast<uint32_t>(
        (xTaskGetTickCount() - probe_tick) * portTICK_PERIOD_MS);
    const uint32_t poll_delay_ms =
        probe_elapsed_ms < kCameraSensorReadyPollIntervalMs
            ? kCameraSensorReadyPollIntervalMs - probe_elapsed_ms
            : 0;
    const uint32_t delay_ms =
        std::min(poll_delay_ms, camera_utils::StartupRemainingMs(startup_tick));
    if (delay_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
  }

  if (camera_preview_.stop_requested.load()) {
    return false;
  }
  camera_preview_.error.store(CameraError::kSensorNotDetected);
  LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
      "Camera sensor readiness check timed out (address: %#X, attempts: %lu, "
      "timeout: %lu ms)\n",
      kCameraSensorI2cAddress, static_cast<unsigned long>(attempt),
      static_cast<unsigned long>(kCameraStartupTimeoutMs));
  return false;
}

bool TDisplayP4Device::InitializeCameraPreview() {
  if (!driver_.IsScreenReady()) {
    camera_preview_.error.store(CameraError::kScreenNotReady);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Camera preview start failed: screen is not ready\n");
    return false;
  }

  const TickType_t startup_tick = xTaskGetTickCount();
  uint32_t attempt = 0;
  CameraError last_error = CameraError::kNone;
  camera_preview_.startup_in_progress.store(true);
  while (!camera_preview_.stop_requested.load() &&
         !camera_utils::StartupTimedOut(startup_tick)) {
    ++attempt;
    camera_preview_.error.store(CameraError::kNone);
    const CameraStartupAttemptResult result =
        InitializeCameraPreviewAttempt(startup_tick);
    if (result == CameraStartupAttemptResult::kSuccess) {
      camera_preview_.startup_in_progress.store(false);
      if (attempt > 1) {
        LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
            "Camera startup recovered (attempts: %lu, elapsed: %lu ms)\n",
            static_cast<unsigned long>(attempt),
            static_cast<unsigned long>(
                camera_utils::StartupElapsedMs(startup_tick)));
      }
      return true;
    }

    last_error = camera_preview_.error.load();
    if (camera_preview_.stop_requested.load()) {
      camera_preview_.startup_in_progress.store(false);
      return false;
    }
    if (result == CameraStartupAttemptResult::kStop) {
      camera_preview_.startup_in_progress.store(false);
      return false;
    }
    if (camera_utils::StartupTimedOut(startup_tick)) {
      break;
    }

    const DiagnosticError diagnostic_error =
        GetCameraDiagnosticError(last_error);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Camera startup attempt failed; retrying from power-on (attempt: %lu, "
        "error: %s, reason: %s, elapsed: %lu ms)\n",
        static_cast<unsigned long>(attempt), diagnostic_error.code,
        diagnostic_error.text,
        static_cast<unsigned long>(
            camera_utils::StartupElapsedMs(startup_tick)));
    DeinitializeCameraPreview();

    const uint32_t delay_ms = std::min(kCameraPowerCycleOffDelayMs,
        camera_utils::StartupRemainingMs(startup_tick));
    if (delay_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
  }

  if (last_error == CameraError::kNone) {
    last_error = CameraError::kSensorNotDetected;
  }
  camera_preview_.error.store(last_error);
  camera_preview_.startup_in_progress.store(false);
  const DiagnosticError diagnostic_error = GetCameraDiagnosticError(last_error);
  LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
      "Camera startup timed out (attempts: %lu, elapsed: %lu ms, error: %s, "
      "reason: %s)\n",
      static_cast<unsigned long>(attempt),
      static_cast<unsigned long>(camera_utils::StartupElapsedMs(startup_tick)),
      diagnostic_error.code, diagnostic_error.text);
  return false;
}

TDisplayP4Device::CameraStartupAttemptResult
TDisplayP4Device::InitializeCameraPreviewAttempt(TickType_t startup_tick) {
  if (!driver_.SetCameraPowerEnabled(true)) {
    camera_preview_.error.store(CameraError::kPowerEnableFailed);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Camera preview start failed: power enable failed\n");
    return CameraStartupAttemptResult::kPowerCycle;
  }
  if (!WaitForCameraSensorReady(startup_tick)) {
    return CameraStartupAttemptResult::kStop;
  }

  const bool video_system_was_initialized =
      camera_preview_.video_system_initialized.load();
  bool initialized_video_system = false;
  if (!video_system_was_initialized) {
    esp_video_init_csi_config_t csi_config = {};
    csi_config.sccb_config.init_sccb = false;
    csi_config.sccb_config.i2c_handle =
        driver_.bus().sgm38121_i2c_bus->bus_handle();
    csi_config.sccb_config.freq = static_cast<uint32_t>(100000);
    csi_config.reset_pin = GPIO_NUM_NC;
    csi_config.pwdn_pin = GPIO_NUM_NC;
    csi_config.dont_init_ldo = true;

    esp_video_init_config_t camera_config = {};
    camera_config.csi = &csi_config;
    // video0 和 video20 只注册一次，避免组件反初始化后无法重复注册 VFS。
    esp_err_t result =
        esp_video_init_with_flags(&camera_config, kCameraVideoInitFlags);
    if (result != ESP_OK) {
      // 组件初始化失败时会清理当前已经创建的全部视频设备。
      camera_preview_.video_system_initialized.store(false);
      camera_preview_.error.store(CameraError::kVideoInitFailed);
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "esp_video_init_with_flags failed: %s (%#X)\n",
          esp_err_to_name(result), static_cast<unsigned>(result));
      return camera_utils::IsRetryableVideoError(result)
                 ? CameraStartupAttemptResult::kPowerCycle
                 : CameraStartupAttemptResult::kStop;
    }
    initialized_video_system = true;
  }

  camera_preview_.video_fd = open(kCameraDeviceName, O_RDONLY | O_NONBLOCK);
  if (camera_preview_.video_fd < 0) {
    const int open_error = errno;
    if (open_error == ENOENT) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Open camera video device failed (device: %s, reason: video device "
          "node is unavailable, errno: %d)\n",
          kCameraDeviceName, open_error);
    } else {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Open camera video device failed (device: %s, reason: %s, errno: "
          "%d)\n",
          kCameraDeviceName, std::strerror(open_error), open_error);
    }
    esp_err_t deinit_result = ESP_OK;
    if (!video_system_was_initialized) {
      deinit_result = esp_video_deinit_with_flags(kCameraVideoInitFlags);
      camera_preview_.video_system_initialized.store(false);
    }
    camera_preview_.error.store(CameraError::kVideoDeviceOpenFailed);
    if (deinit_result != ESP_OK) {
      camera_preview_.error.store(CameraError::kVideoInitFailed);
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "esp_video_deinit_with_flags failed: %s (%#X)\n",
          esp_err_to_name(deinit_result), static_cast<unsigned>(deinit_result));
      return CameraStartupAttemptResult::kStop;
    }
    return camera_utils::IsRetryableIoError(open_error)
               ? CameraStartupAttemptResult::kPowerCycle
               : CameraStartupAttemptResult::kStop;
  }
  if (initialized_video_system) {
    camera_preview_.video_system_initialized.store(true);
  }

  if (video_system_was_initialized) {
    // 摄像头重新上电后，通过组件公开接口重写传感器的完整寄存器配置。
    auto& sensor_format = camera_preview_.sensor_format;
    if (ioctl(camera_preview_.video_fd, VIDIOC_G_SENSOR_FMT, &sensor_format) !=
        0) {
      const int ioctl_error = errno;
      camera_preview_.error.store(CameraError::kSensorRestoreFailed);
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "VIDIOC_G_SENSOR_FMT failed (reason: %s, errno: %d)\n",
          std::strerror(ioctl_error), ioctl_error);
      return camera_utils::IsRetryableIoError(ioctl_error)
                 ? CameraStartupAttemptResult::kPowerCycle
                 : CameraStartupAttemptResult::kStop;
    }
    if (ioctl(camera_preview_.video_fd, VIDIOC_S_SENSOR_FMT, &sensor_format) !=
        0) {
      const int ioctl_error = errno;
      camera_preview_.error.store(CameraError::kSensorRestoreFailed);
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "VIDIOC_S_SENSOR_FMT failed (reason: %s, errno: %d)\n",
          std::strerror(ioctl_error), ioctl_error);
      return camera_utils::IsRetryableIoError(ioctl_error)
                 ? CameraStartupAttemptResult::kPowerCycle
                 : CameraStartupAttemptResult::kStop;
    }
  }

  v4l2_format format = {};
  format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(camera_preview_.video_fd, VIDIOC_G_FMT, &format) != 0) {
    const int ioctl_error = errno;
    camera_preview_.error.store(CameraError::kFormatConfigurationFailed);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "VIDIOC_G_FMT failed (reason: %s, errno: %d)\n",
        std::strerror(ioctl_error), ioctl_error);
    return camera_utils::IsRetryableIoError(ioctl_error)
               ? CameraStartupAttemptResult::kPowerCycle
               : CameraStartupAttemptResult::kStop;
  }
  camera_preview_.frame_width = format.fmt.pix.width;
  camera_preview_.frame_height = format.fmt.pix.height;
#if defined(CONFIG_LILYGO_DEVICE_DRIVER_CAMERA_TYPE_OV5645)
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
#elif defined(CONFIG_LILYGO_DEVICE_DRIVER_SCREEN_PIXEL_FORMAT_RGB888)
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB24;
#else
  format.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
#endif
  if (ioctl(camera_preview_.video_fd, VIDIOC_S_FMT, &format) != 0) {
    const int ioctl_error = errno;
    camera_preview_.error.store(CameraError::kFormatConfigurationFailed);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "VIDIOC_S_FMT failed (reason: %s, errno: %d)\n",
        std::strerror(ioctl_error), ioctl_error);
    return camera_utils::IsRetryableIoError(ioctl_error)
               ? CameraStartupAttemptResult::kPowerCycle
               : CameraStartupAttemptResult::kStop;
  }
  camera_preview_.frame_width = format.fmt.pix.width;
  camera_preview_.frame_height = format.fmt.pix.height;

  v4l2_requestbuffers request = {};
  request.count = kCameraBufferCount;
  request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  request.memory = V4L2_MEMORY_MMAP;
  if (ioctl(camera_preview_.video_fd, VIDIOC_REQBUFS, &request) != 0) {
    camera_preview_.error.store(CameraError::kBufferAllocationFailed);
    LogMessage(
        LogLevel::kWarning, __FILE__, __LINE__, "VIDIOC_REQBUFS failed\n");
    return CameraStartupAttemptResult::kStop;
  }
  if (request.count < kCameraBufferCount) {
    camera_preview_.error.store(CameraError::kBufferAllocationFailed);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "VIDIOC_REQBUFS returned too few buffers: %lu\n",
        static_cast<unsigned long>(request.count));
    return CameraStartupAttemptResult::kStop;
  }

  for (uint32_t index = 0; index < kCameraBufferCount; ++index) {
    v4l2_buffer buffer = {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    if (ioctl(camera_preview_.video_fd, VIDIOC_QUERYBUF, &buffer) != 0) {
      camera_preview_.error.store(CameraError::kBufferAllocationFailed);
      LogMessage(
          LogLevel::kWarning, __FILE__, __LINE__, "VIDIOC_QUERYBUF failed\n");
      return CameraStartupAttemptResult::kStop;
    }
    camera_preview_.frame_buffer_sizes[index] = buffer.length;
    camera_preview_.frame_buffers[index] =
        mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED,
            camera_preview_.video_fd, buffer.m.offset);
    if (camera_preview_.frame_buffers[index] == MAP_FAILED) {
      camera_preview_.frame_buffers[index] = nullptr;
      camera_preview_.error.store(CameraError::kBufferMappingFailed);
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Camera buffer mmap failed\n");
      return CameraStartupAttemptResult::kStop;
    }
    if (ioctl(camera_preview_.video_fd, VIDIOC_QBUF, &buffer) != 0) {
      camera_preview_.error.store(CameraError::kBufferAllocationFailed);
      LogMessage(
          LogLevel::kWarning, __FILE__, __LINE__, "VIDIOC_QBUF failed\n");
      return CameraStartupAttemptResult::kStop;
    }
  }

  if (camera_preview_.output_mutex == nullptr) {
    camera_preview_.output_mutex = xSemaphoreCreateMutex();
    if (camera_preview_.output_mutex == nullptr) {
      camera_preview_.error.store(CameraError::kOutputBufferAllocationFailed);
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Camera output mutex allocation failed\n");
      return CameraStartupAttemptResult::kStop;
    }
  }

  if (!camera_preview_.ppa.Init()) {
    camera_preview_.error.store(CameraError::kProcessingInitFailed);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__, "PPA SRM init failed\n");
    return CameraStartupAttemptResult::kStop;
  }
  const size_t bytes_per_pixel = ScreenBitsPerPixel() / 8;
  camera_preview_.output_rotation_angle =
      camera_utils::NormalizePreviewRotationAngle(
          app::GetDisplayPreferences().screen_rotation_angle);
  const bool output_rotated = camera_preview_.output_rotation_angle == 90 ||
                              camera_preview_.output_rotation_angle == 270;
  const uint32_t output_screen_width =
      output_rotated ? ScreenHeight() : ScreenWidth();
  const uint32_t output_screen_height =
      output_rotated ? ScreenWidth() : ScreenHeight();
  // SurvivalOS: both buffers are allocated for the largest geometry, the
  // whole screen, so that switching the zoom never has to reallocate them.
  camera_preview_.output_buffer_size =
      AlignUp(output_screen_width * output_screen_height * bytes_per_pixel,
          camera_preview_.ppa.CacheLineSize());
  void* output_buffer =
      heap_caps_aligned_calloc(camera_preview_.ppa.CacheLineSize(), 1,
          camera_preview_.output_buffer_size, MALLOC_CAP_SPIRAM);
  if (output_buffer == nullptr) {
    camera_preview_.error.store(CameraError::kOutputBufferAllocationFailed);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Camera output buffer allocation failed\n");
    return CameraStartupAttemptResult::kStop;
  }
  camera_preview_.output_buffer.reset(static_cast<uint8_t*>(output_buffer));
  void* output_back_buffer =
      heap_caps_aligned_calloc(camera_preview_.ppa.CacheLineSize(), 1,
          camera_preview_.output_buffer_size, MALLOC_CAP_SPIRAM);
  if (output_back_buffer == nullptr) {
    camera_preview_.output_buffer.reset();
    camera_preview_.error.store(CameraError::kOutputBufferAllocationFailed);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Camera output back buffer allocation failed\n");
    return CameraStartupAttemptResult::kStop;
  }
  camera_preview_.output_back_buffer.reset(
      static_cast<uint8_t*>(output_back_buffer));
  camera_preview_.applied_zoom = 0xFF;
  ApplyCameraPreviewGeometry();
  camera_preview_.warmup_frames_remaining = kCameraWarmupFrameCount;

  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(camera_preview_.video_fd, VIDIOC_STREAMON, &type) != 0) {
    const int ioctl_error = errno;
    camera_preview_.error.store(CameraError::kStreamStartFailed);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "VIDIOC_STREAMON failed (reason: %s, errno: %d)\n",
        std::strerror(ioctl_error), ioctl_error);
    return camera_utils::IsRetryableIoError(ioctl_error)
               ? CameraStartupAttemptResult::kPowerCycle
               : CameraStartupAttemptResult::kStop;
  }

  camera_preview_.initialized.store(true);
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "Camera preview started (%lux%lu)\n", camera_preview_.frame_width,
      camera_preview_.frame_height);
  return CameraStartupAttemptResult::kSuccess;
}

void TDisplayP4Device::DeinitializeCameraPreview() {
  if (camera_preview_.video_fd >= 0) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(camera_preview_.video_fd, VIDIOC_STREAMOFF, &type);
  }
  for (uint32_t index = 0; index < kCameraBufferCount; ++index) {
    if (camera_preview_.frame_buffers[index] != nullptr) {
      munmap(camera_preview_.frame_buffers[index],
          camera_preview_.frame_buffer_sizes[index]);
      camera_preview_.frame_buffers[index] = nullptr;
      camera_preview_.frame_buffer_sizes[index] = 0;
    }
  }
  if (camera_preview_.video_fd >= 0) {
    // 显式归还驱动缓冲区，避免下次打开预览时残留旧缓冲状态
    v4l2_requestbuffers request = {};
    request.count = 0;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl(camera_preview_.video_fd, VIDIOC_REQBUFS, &request) != 0) {
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "VIDIOC_REQBUFS release failed\n");
    }
    close(camera_preview_.video_fd);
    camera_preview_.video_fd = -1;
  }
  camera_preview_.output_buffer.reset();
  camera_preview_.output_back_buffer.reset();
  camera_preview_.output_fill = false;
  camera_preview_.applied_zoom = 0xFF;
  camera_preview_.output_frame_size = 0;
  if (camera_preview_.output_mutex != nullptr) {
    vSemaphoreDelete(camera_preview_.output_mutex);
    camera_preview_.output_mutex = nullptr;
  }
  camera_preview_.output_buffer_size = 0;
  camera_preview_.output_width = 0;
  camera_preview_.output_height = 0;
  camera_preview_.output_stride = 0;
  camera_preview_.output_rotation_angle = 0;
  camera_preview_.clear_output_frames_remaining = 0;
  camera_preview_.warmup_frames_remaining = 0;
  camera_preview_.frame_sequence.store(0);
  camera_preview_.initialized.store(false);
  camera_preview_.ppa.Deinit();

  // 保留 video0/video20 的 VFS 节点，只关闭摄像头供电以降低页面退出后的功耗。
  if (!driver_.SetCameraPowerEnabled(false)) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Camera power disable failed\n");
  }
}

int TDisplayP4Device::CameraPreviewZoomStepCount() const {
  return static_cast<int>(camera_utils::PreviewZoom::kFill) + 1;
}

int TDisplayP4Device::GetCameraPreviewZoomStep() const {
  return static_cast<int>(camera_utils::GetPreviewZoom());
}

bool TDisplayP4Device::SetCameraPreviewZoomStep(int step) {
  if (step < 0 || step >= CameraPreviewZoomStepCount()) {
    return false;
  }
  camera_utils::SetPreviewZoom(static_cast<camera_utils::PreviewZoom>(step));
  return true;
}

const char* TDisplayP4Device::CameraPreviewZoomLabel(int step) const {
  if (step < 0 || step >= CameraPreviewZoomStepCount()) {
    return "";
  }
  return camera_utils::PreviewZoomLabel(
      static_cast<camera_utils::PreviewZoom>(step));
}

void TDisplayP4Device::ApplyCameraPreviewGeometry() {
  // The sensor frame is landscape and the screen portrait, so the preview
  // shows a centre crop of the frame scaled into a buffer of its own size:
  // the whole frame as a strip (kWide), its full height in a roughly square
  // viewfinder (kNormal), or as much as covers the screen (kFill). The bigger
  // the share of the frame, the smaller the viewfinder.
  //
  // Buffer sides are multiples of 16 because photos and video are JPEG-encoded
  // straight from the buffer in 16x16 blocks. The PPA scales in sixteenths, so
  // the factor is rounded up to the next sixteenth and the crop is the largest
  // one that still lands inside the buffer.
  constexpr uint32_t kJpegBlock = 16;
  constexpr uint32_t kPpaScaleUnit = 16;
  const auto divide_rounding_up = [](uint32_t value, uint32_t divisor) {
    return (value + divisor - 1) / divisor;
  };
  const camera_utils::PreviewZoom zoom = camera_utils::GetPreviewZoom();
  const uint32_t frame_width = camera_preview_.frame_width;
  const uint32_t frame_height = camera_preview_.frame_height;
  const bool output_rotated = camera_preview_.output_rotation_angle == 90 ||
                              camera_preview_.output_rotation_angle == 270;
  const uint32_t screen_width = output_rotated ? ScreenHeight() : ScreenWidth();
  const uint32_t screen_height =
      output_rotated ? ScreenWidth() : ScreenHeight();
  const uint32_t box_width = screen_width / kJpegBlock * kJpegBlock;
  const uint32_t box_height = screen_height / kJpegBlock * kJpegBlock;

  // The vendor's geometry: the whole frame fitted into a screen-sized buffer.
  bool fill = false;
  uint32_t output_width = std::max<uint32_t>(1, screen_width);
  uint32_t output_height = std::max<uint32_t>(1, screen_height);
  uint32_t scale = kPpaScaleUnit;
  uint32_t crop_x = 0;
  uint32_t crop_y = 0;
  uint32_t crop_width = 0;
  uint32_t crop_height = 0;
  uint32_t offset_x = 0;
  uint32_t offset_y = 0;
  if (!output_rotated && frame_width > frame_height && frame_height > 0 &&
      box_width > 0 && box_height > 0) {
    const uint32_t scale_to_width =
        divide_rounding_up(box_width * kPpaScaleUnit, frame_width);
    switch (zoom) {
      case camera_utils::PreviewZoom::kWide:
        scale = scale_to_width;
        break;
      case camera_utils::PreviewZoom::kNormal:
        scale = divide_rounding_up(box_width * kPpaScaleUnit, frame_height);
        break;
      case camera_utils::PreviewZoom::kFill:
        scale = std::max(scale_to_width,
            divide_rounding_up(box_height * kPpaScaleUnit, frame_height));
        break;
    }
    scale = std::max<uint32_t>(1, scale);
    crop_width = std::min<uint32_t>(
        frame_width, (box_width * kPpaScaleUnit / scale) & ~1U);
    crop_height = std::min<uint32_t>(
        frame_height, (box_height * kPpaScaleUnit / scale) & ~1U);
    const uint32_t scaled_width = crop_width * scale / kPpaScaleUnit;
    const uint32_t scaled_height = crop_height * scale / kPpaScaleUnit;
    const uint32_t fitted_width = std::min(
        box_width, divide_rounding_up(scaled_width, kJpegBlock) * kJpegBlock);
    const uint32_t fitted_height = std::min(box_height,
        divide_rounding_up(scaled_height, kJpegBlock) * kJpegBlock);
    fill = crop_width > 0 && crop_height > 0 && scaled_width > 0 &&
           scaled_height > 0 && scaled_width <= fitted_width &&
           scaled_height <= fitted_height;
    if (fill) {
      output_width = fitted_width;
      output_height = fitted_height;
      crop_x = ((frame_width - crop_width) / 2) & ~1U;
      crop_y = ((frame_height - crop_height) / 2) & ~1U;
      // Rounding can leave the picture a pixel or two short of the buffer.
      offset_x = (fitted_width - scaled_width) / 2;
      offset_y = (fitted_height - scaled_height) / 2;
    }
  }

  const size_t bytes_per_pixel = ScreenBitsPerPixel() / 8;
  const bool locked = camera_preview_.output_mutex != nullptr &&
      xSemaphoreTake(camera_preview_.output_mutex, portMAX_DELAY) == pdTRUE;
  camera_preview_.output_fill = fill;
  camera_preview_.fill_scale_sixteenths = scale;
  camera_preview_.input_crop_x = crop_x;
  camera_preview_.input_crop_y = crop_y;
  camera_preview_.input_crop_width = crop_width;
  camera_preview_.input_crop_height = crop_height;
  camera_preview_.fill_offset_x = offset_x;
  camera_preview_.fill_offset_y = offset_y;
  camera_preview_.output_width = output_width;
  camera_preview_.output_height = output_height;
  camera_preview_.output_stride = output_width * bytes_per_pixel;
  camera_preview_.output_frame_size =
      std::min(camera_preview_.output_buffer_size,
          AlignUp(camera_preview_.output_stride * output_height,
              camera_preview_.ppa.CacheLineSize()));
  // Both buffers still hold the old picture around the new one.
  camera_preview_.clear_output_frames_remaining = kCameraOutputClearFrameCount;
  camera_preview_.applied_zoom = static_cast<uint8_t>(zoom);
  if (locked) {
    xSemaphoreGive(camera_preview_.output_mutex);
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "Camera preview %lux%lu from a %lux%lu crop at (%lu,%lu), scale %lu/16\n",
      static_cast<unsigned long>(output_width),
      static_cast<unsigned long>(output_height),
      static_cast<unsigned long>(crop_width),
      static_cast<unsigned long>(crop_height),
      static_cast<unsigned long>(crop_x), static_cast<unsigned long>(crop_y),
      static_cast<unsigned long>(scale));
}

bool TDisplayP4Device::RenderCameraFrame(
    uint8_t* buffer, uint32_t width, uint32_t height) {
  if (buffer == nullptr || camera_preview_.output_buffer == nullptr ||
      camera_preview_.output_back_buffer == nullptr ||
      camera_preview_.output_mutex == nullptr) {
    return false;
  }

  const bool fill = camera_preview_.output_fill;
  const uint32_t output_width = camera_preview_.output_width;
  const uint32_t output_height = camera_preview_.output_height;
  const int output_rotation_angle = camera_preview_.output_rotation_angle;
  const bool output_rotated =
      output_rotation_angle == 90 || output_rotation_angle == 270;
  const uint32_t rotated_source_width = output_rotated ? height : width;
  const uint32_t rotated_source_height = output_rotated ? width : height;
  const float scale = fill
      ? static_cast<float>(camera_preview_.fill_scale_sixteenths) / 16.0F
      : std::min(static_cast<float>(output_width) /
                     static_cast<float>(rotated_source_width),
            static_cast<float>(output_height) /
                static_cast<float>(rotated_source_height));
  const uint32_t scaled_width = std::max<uint32_t>(
      1, static_cast<uint32_t>(std::round(rotated_source_width * scale)));
  const uint32_t scaled_height = std::max<uint32_t>(
      1, static_cast<uint32_t>(std::round(rotated_source_height * scale)));
  const uint32_t output_offset_x = fill
      ? camera_preview_.fill_offset_x
      : output_width > scaled_width ? (output_width - scaled_width) / 2 : 0;
  const uint32_t output_offset_y = fill
      ? camera_preview_.fill_offset_y
      : output_height > scaled_height ? (output_height - scaled_height) / 2
                                      : 0;
  const size_t aligned_output_size = camera_preview_.output_buffer_size;
#if defined(CONFIG_LILYGO_DEVICE_DRIVER_CAMERA_TYPE_OV5645)
  const ppa_srm_color_mode_t input_color_mode = PPA_SRM_COLOR_MODE_RGB565;
#elif defined(CONFIG_LILYGO_DEVICE_DRIVER_SCREEN_PIXEL_FORMAT_RGB888)
  const ppa_srm_color_mode_t input_color_mode = PPA_SRM_COLOR_MODE_RGB888;
#else
  const ppa_srm_color_mode_t input_color_mode = PPA_SRM_COLOR_MODE_RGB565;
#endif
#if defined(CONFIG_LILYGO_DEVICE_DRIVER_SCREEN_PIXEL_FORMAT_RGB888)
  const ppa_srm_color_mode_t output_color_mode = PPA_SRM_COLOR_MODE_RGB888;
#else
  const ppa_srm_color_mode_t output_color_mode = PPA_SRM_COLOR_MODE_RGB565;
#endif
  PpaSrmImageConfig input = {
      .buffer = buffer,
      .pic_width = width,
      .pic_height = height,
      .block_width = fill ? camera_preview_.input_crop_width : width,
      .block_height = fill ? camera_preview_.input_crop_height : height,
      .block_offset_x = fill ? camera_preview_.input_crop_x : 0,
      .block_offset_y = fill ? camera_preview_.input_crop_y : 0,
      .color_mode = input_color_mode,
  };
  // Render into the back buffer, which only this task touches, so the UI can
  // keep copying the previous frame out of output_buffer meanwhile.
  uint8_t* const target = camera_preview_.output_back_buffer.get();
  PpaSrmImageConfig output = {
      .buffer = target,
      .buffer_size = aligned_output_size,
      .pic_width = output_width,
      .pic_height = output_height,
      .block_width = output_width,
      .block_height = output_height,
      .block_offset_x = output_offset_x,
      .block_offset_y = output_offset_y,
      .color_mode = output_color_mode,
  };
  PpaSrmTransformConfig transform = {
      .rotation_angle =
          camera_utils::ToPreviewPpaRotation(output_rotation_angle),
      .scale_x = scale,
      .scale_y = scale,
      .mirror_y = driver_.screen_type() == device::ScreenType::kHi8561,
  };
  // When filling, any uncovered pixel is a fixed border that only has to be
  // blanked once per buffer; the fit-to-screen path keeps upstream's behaviour
  // of clearing its bars on every frame.
  if (camera_preview_.clear_output_frames_remaining > 0 ||
      (!fill && (output_offset_x > 0 || output_offset_y > 0))) {
    std::memset(target, 0, camera_preview_.output_buffer_size);
    if (camera_preview_.clear_output_frames_remaining > 0) {
      --camera_preview_.clear_output_frames_remaining;
    }
  }
  if (!camera_preview_.ppa.Transform(input, output, transform)) {
    // Once is enough: a failure here repeats with every frame.
    static bool failure_logged = false;
    if (!failure_logged) {
      failure_logged = true;
      LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
          "Camera frame scaling failed\n");
    }
    return false;
  }
  // Publishing the frame is only a pointer swap, so the lock is held for
  // microseconds instead of for a whole render.
  if (xSemaphoreTake(camera_preview_.output_mutex, pdMS_TO_TICKS(20)) !=
      pdTRUE) {
    return false;
  }
  camera_preview_.output_buffer.swap(camera_preview_.output_back_buffer);
  camera_preview_.frame_sequence.fetch_add(1);
  xSemaphoreGive(camera_preview_.output_mutex);
  return true;
}

bool TDisplayP4Device::CaptureCameraPhoto(const char* path) {
  if (path == nullptr || path[0] == '\0') {
    return false;
  }

  CameraPreviewFrameInfo info;
  if (!GetCameraPreviewFrameInfo(&info) || info.data_size == 0 ||
      info.width == 0 || info.height == 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "No preview frame to capture\n");
    return false;
  }

  jpeg_enc_input_format_t source_format = JPEG_ENCODE_IN_FORMAT_RGB565;
  if (info.bits_per_pixel == 24) {
    source_format = JPEG_ENCODE_IN_FORMAT_RGB888;
  } else if (info.bits_per_pixel != 16) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Unsupported preview pixel depth for capture: %u\n",
        static_cast<unsigned int>(info.bits_per_pixel));
    return false;
  }

  // The encoder wants its buffers from a specific memory region, so the frame
  // is copied into a buffer it allocated rather than reusing the preview one.
  const jpeg_encode_memory_alloc_cfg_t input_config = {
      .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
  };
  size_t input_capacity = 0;
  auto* input_buffer = static_cast<uint8_t*>(
      jpeg_alloc_encoder_mem(info.data_size, &input_config, &input_capacity));
  if (input_buffer == nullptr) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Allocate capture input buffer failed (%u bytes)\n",
        static_cast<unsigned int>(info.data_size));
    return false;
  }

  CameraPreviewFrameInfo copied;
  if (!CopyCameraPreviewFrame(input_buffer, input_capacity, &copied)) {
    free(input_buffer);
    return false;
  }

  // A quality-90 JPEG never approaches the raw frame size, so the raw size is a
  // safe upper bound for the compressed output.
  const jpeg_encode_memory_alloc_cfg_t output_config = {
      .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
  };
  size_t output_capacity = 0;
  auto* output_buffer = static_cast<uint8_t*>(
      jpeg_alloc_encoder_mem(copied.data_size, &output_config,
          &output_capacity));
  if (output_buffer == nullptr) {
    free(input_buffer);
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Allocate capture output buffer failed\n");
    return false;
  }

  jpeg_encoder_handle_t encoder = nullptr;
  const jpeg_encode_engine_cfg_t engine_config = {
      .intr_priority = 0,
      .timeout_ms = kCaptureEncodeTimeoutMs,
  };
  esp_err_t error = jpeg_new_encoder_engine(&engine_config, &encoder);
  if (error != ESP_OK) {
    free(input_buffer);
    free(output_buffer);
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Create JPEG encoder failed: %s\n", esp_err_to_name(error));
    return false;
  }

  const jpeg_encode_cfg_t encode_config = {
      .height = copied.height,
      .width = copied.width,
      .src_type = source_format,
      .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
      .image_quality = kCaptureJpegQuality,
      .pixel_reverse = false,
  };
  uint32_t encoded_size = 0;
  error = jpeg_encoder_process(encoder, &encode_config, input_buffer,
      static_cast<uint32_t>(copied.data_size), output_buffer,
      static_cast<uint32_t>(output_capacity), &encoded_size);
  jpeg_del_encoder_engine(encoder);
  free(input_buffer);

  if (error != ESP_OK || encoded_size == 0) {
    free(output_buffer);
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "JPEG encode failed: %s\n", esp_err_to_name(error));
    return false;
  }

  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    free(output_buffer);
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Open photo file failed: %s\n", path);
    return false;
  }
  const size_t written = std::fwrite(output_buffer, 1, encoded_size, file);
  std::fclose(file);
  free(output_buffer);

  if (written != encoded_size) {
    std::remove(path);
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Write photo failed: %s\n", path);
    return false;
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "Captured %ux%u photo, %u bytes -> %s\n",
      static_cast<unsigned int>(copied.width),
      static_cast<unsigned int>(copied.height),
      static_cast<unsigned int>(encoded_size), path);
  return true;
}

bool TDisplayP4Device::StartVideoRecording(const char* path) {
  if (path == nullptr || path[0] == '\0' || video_.running.load()) {
    return false;
  }
  if (std::strlen(path) >= sizeof(video_.path)) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Video path is too long: %s\n", path);
    return false;
  }

  CameraPreviewFrameInfo info;
  if (!GetCameraPreviewFrameInfo(&info) || info.width == 0 ||
      info.height == 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "No preview running, cannot record\n");
    return false;
  }

  std::snprintf(video_.path, sizeof(video_.path), "%s", path);
  video_.stop_requested.store(false);
  video_.failed.store(false);
  video_.frame_count.store(0);
  video_.elapsed_ms.store(0);
  video_.measured_fps.store(0);
  video_.bytes_written.store(0);
  video_.running.store(true);

  if (xTaskCreate(VideoRecordingTaskEntry, "video_rec",
          kVideoTaskStackBytes, this, kVideoTaskPriority, nullptr) != pdPASS) {
    video_.running.store(false);
    return false;
  }
  return true;
}

bool TDisplayP4Device::StopVideoRecording() {
  if (!video_.running.load()) {
    return true;
  }
  video_.stop_requested.store(true);
  return true;
}

bool TDisplayP4Device::ReadVideoRecordingStatus(VideoRecordingStatus* status) {
  if (status == nullptr) {
    return false;
  }
  *status = VideoRecordingStatus();
  if (video_.failed.load()) {
    status->state = VideoRecordingState::kError;
  } else if (video_.running.load()) {
    status->state = VideoRecordingState::kRecording;
  } else if (video_.frame_count.load() > 0) {
    status->state = VideoRecordingState::kCompleted;
  }
  status->elapsed_ms = video_.elapsed_ms.load();
  status->frame_count = video_.frame_count.load();
  status->bytes_written = video_.bytes_written.load();
  status->frames_per_second = video_.measured_fps.load();
  return true;
}

void TDisplayP4Device::VideoRecordingTaskEntry(void* context) {
  auto* self = static_cast<TDisplayP4Device*>(context);
  if (self != nullptr) {
    self->RunVideoRecordingTask();
  }
  vTaskDelete(nullptr);
}

void TDisplayP4Device::RunVideoRecordingTask() {
  CameraPreviewFrameInfo info;
  if (!GetCameraPreviewFrameInfo(&info) || info.data_size == 0) {
    video_.failed.store(true);
    video_.running.store(false);
    return;
  }

  jpeg_enc_input_format_t source_format = JPEG_ENCODE_IN_FORMAT_RGB565;
  if (info.bits_per_pixel == 24) {
    source_format = JPEG_ENCODE_IN_FORMAT_RGB888;
  } else if (info.bits_per_pixel != 16) {
    video_.failed.store(true);
    video_.running.store(false);
    return;
  }

  // The encoder and its buffers are set up once and reused for every frame;
  // allocating per frame would not keep up with the frame rate.
  const jpeg_encode_memory_alloc_cfg_t input_config = {
      .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
  };
  size_t input_capacity = 0;
  auto* input_buffer = static_cast<uint8_t*>(
      jpeg_alloc_encoder_mem(info.data_size, &input_config, &input_capacity));
  const jpeg_encode_memory_alloc_cfg_t output_config = {
      .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
  };
  size_t output_capacity = 0;
  auto* output_buffer = static_cast<uint8_t*>(
      jpeg_alloc_encoder_mem(info.data_size, &output_config,
          &output_capacity));

  jpeg_encoder_handle_t encoder = nullptr;
  const jpeg_encode_engine_cfg_t engine_config = {
      .intr_priority = 0,
      .timeout_ms = kCaptureEncodeTimeoutMs,
  };
  survival::AviWriter writer;

  if (input_buffer == nullptr || output_buffer == nullptr ||
      jpeg_new_encoder_engine(&engine_config, &encoder) != ESP_OK ||
      !writer.Open(video_.path, info.width, info.height, kVideoTargetFps)) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Could not start recording %s\n", video_.path);
    if (encoder != nullptr) {
      jpeg_del_encoder_engine(encoder);
    }
    free(input_buffer);
    free(output_buffer);
    video_.failed.store(true);
    video_.running.store(false);
    return;
  }

  const uint32_t started_ms =
      static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS);
  TickType_t next_frame = xTaskGetTickCount();
  uint32_t last_sequence = 0xFFFFFFFFU;

  while (!video_.stop_requested.load()) {
    CameraPreviewFrameInfo current;
    if (GetCameraPreviewFrameInfo(&current) &&
        current.sequence != last_sequence &&
        CopyCameraPreviewFrame(input_buffer, input_capacity, &current)) {
      last_sequence = current.sequence;

      const jpeg_encode_cfg_t encode_config = {
          .height = current.height,
          .width = current.width,
          .src_type = source_format,
          .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
          .image_quality = kVideoJpegQuality,
          .pixel_reverse = false,
      };
      uint32_t encoded_size = 0;
      if (jpeg_encoder_process(encoder, &encode_config, input_buffer,
              static_cast<uint32_t>(current.data_size), output_buffer,
              static_cast<uint32_t>(output_capacity), &encoded_size) ==
              ESP_OK &&
          encoded_size > 0) {
        if (!writer.AddFrame(output_buffer, encoded_size)) {
          // The container is full or the card failed; stopping cleanly keeps
          // what has been recorded so far playable.
          break;
        }
        video_.frame_count.store(writer.frame_count());
        video_.bytes_written.store(writer.bytes_written());
      }
    }

    const uint32_t elapsed =
        static_cast<uint32_t>(xTaskGetTickCount() * portTICK_PERIOD_MS) -
        started_ms;
    video_.elapsed_ms.store(elapsed);
    if (elapsed > 0) {
      video_.measured_fps.store(writer.frame_count() * 1000U / elapsed);
    }
    xTaskDelayUntil(&next_frame, pdMS_TO_TICKS(kVideoFramePeriodMs));
  }

  const bool closed = writer.Close();
  jpeg_del_encoder_engine(encoder);
  free(input_buffer);
  free(output_buffer);
  if (!closed) {
    video_.failed.store(true);
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__,
      "Recorded %u frames to %s\n",
      static_cast<unsigned int>(video_.frame_count.load()), video_.path);
  video_.running.store(false);
}

}  // namespace lilygo_box::hal
