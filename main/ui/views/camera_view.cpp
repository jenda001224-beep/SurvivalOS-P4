/*
 * @Description: Camera app view
 * @Author: LILYGO_L
 * @Date: 2026-07-02 00:00:00
 * @LastEditTime: 2026-09-02 17:54:27
 * @License: GPL 3.0
 */
#include "ui/views/camera_view.h"
#include <sys/stat.h>
#include <cstring>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <new>

#include "base/logger.h"
#include "ui/haptic_feedback.h"
#include "app/edition.h"
#include "hal/providers/storage_provider.h"
#include "hal/providers/rtc_provider.h"
#include "esp_heap_caps.h"
#include "hal/providers/camera_provider.h"
#include "ui/input/edge_swipe_indicator.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"
#include "ui/widgets/prompt/prompt_status.h"

namespace lilygo_box::ui {
namespace {

// Photos land in the standard DCIM folder so any card reader finds them.
constexpr const char* kPhotoDirectory = "DCIM";
// Upper end of a full-screen JPEG at quality 90, for the internal-memory check.
constexpr size_t kPhotoBytes = 320 * 1024;
constexpr uint32_t kRecordingTimerMs = 200;
constexpr int kModeButtonSize = 72;
constexpr int kShutterButtonSize = 116;
constexpr int kShutterBottomMargin = 64;
constexpr uint32_t kShutterFlashMs = 180;
constexpr uint32_t kCaptureToastMs = 1600;

constexpr uint32_t kCameraViewRefreshPeriodMs = 10;
constexpr int kCameraPromptGroupHeight = 300;
constexpr int kCameraPromptGroupOffsetY = -144;

enum class CameraContentState : uint8_t {
  kScanning,
  kPreview,
  kError,
};

struct CameraViewState {
  hal::CameraProvider* camera = nullptr;
  hal::StorageProvider* storage = nullptr;
  hal::RtcProvider* rtc = nullptr;
  lv_obj_t* shutter_button = nullptr;
  lv_obj_t* shutter_inner = nullptr;
  lv_obj_t* shutter_flash = nullptr;
  lv_obj_t* capture_toast = nullptr;
  // One-shot timer hiding the toast; deleted with the view, because firing
  // after the view is gone would touch a freed label.
  lv_timer_t* toast_timer = nullptr;
  lv_obj_t* mode_button = nullptr;
  lv_obj_t* mode_label = nullptr;
  lv_obj_t* zoom_button = nullptr;
  lv_obj_t* zoom_label = nullptr;
  lv_obj_t* recording_label = nullptr;
  lv_timer_t* recording_timer = nullptr;
  bool video_mode = false;
  bool recording = false;
  std::function<void(std::function<void(bool visible)> callback)>
      set_lock_screen_visibility_callback;
  std::function<void(uint32_t color)> set_status_bar_text_color;
  lv_obj_t* container = nullptr;
  lv_obj_t* status_layer = nullptr;
  lv_obj_t* image = nullptr;
  lv_timer_t* refresh_timer = nullptr;
  lv_image_dsc_t image_dsc = {};
  uint8_t* frame_buffer = nullptr;
  size_t frame_buffer_size = 0;
  CameraContentState content_state = CameraContentState::kScanning;
  CameraError displayed_error = CameraError::kNone;
  bool lock_screen_paused = false;
  bool preview_started = false;
  bool dismiss_stop_requested = false;
  int width = 0;
  int height = 0;
  uint32_t last_sequence = 0;
  uint32_t last_width = 0;
  uint32_t last_height = 0;
  uint32_t last_stride = 0;
  uint32_t last_bits_per_pixel = 0;
};

/**
 * @brief 获取页面使用的 Google Sans 字体
 */
const lv_font_t* Font22() { return &lvgl_font_google_sans_flex_22; }
const lv_font_t* Font28() { return &lvgl_font_google_sans_flex_28; }

/**
 * @brief 将对象设置为透明无边框容器
 * @param object LVGL 对象
 */
/**
 * @brief Builds the path for a new photo from the RTC clock.
 * @param state Camera view state.
 * @param buffer Output buffer.
 * @param size Buffer size.
 * @return true when a path could be built.
 */
bool BuildPhotoPath(CameraViewState* state, char* buffer, size_t size,
    bool* on_card = nullptr) {
  // Mounts the card if nothing has yet; only checking would report "No SD
  // card" for a perfectly good card that no other app happened to open first.
  // SurvivalOS: without a card (MINI, or FULL with the card out) the photo
  // goes to the phone's own memory as long as there is room. MINI does not
  // try the card at all, which would stall the shutter for a second.
  bool card = false;
  const char* base = app::MediaBasePath(state->storage,
      app::GetEdition() == app::Edition::kFull, &card);
  if (on_card != nullptr) {
    *on_card = card;
  }
  if (base == nullptr || (!card && !app::InternalMediaHasRoom(kPhotoBytes))) {
    return false;
  }
  char directory[96] = {};
  if (std::snprintf(directory, sizeof(directory), "%s/%s", base,
          kPhotoDirectory) >= static_cast<int>(sizeof(directory))) {
    return false;
  }
  mkdir(directory, 0777);

  hal::RtcStatus status;
  if (state->rtc != nullptr && state->rtc->ReadRtcStatus(&status) &&
      status.ready && status.year >= 2000) {
    return std::snprintf(buffer, size, "%s/IMG_%04d%02d%02d-%02d%02d%02d.jpg",
               directory, static_cast<int>(status.year),
               static_cast<int>(status.month), static_cast<int>(status.day),
               static_cast<int>(status.hour), static_cast<int>(status.minute),
               static_cast<int>(status.second)) < static_cast<int>(size);
  }
  // Without a valid clock fall back to a counter so names stay unique.
  for (unsigned index = 1; index < 10000; ++index) {
    if (std::snprintf(buffer, size, "%s/IMG_%04u.jpg", directory, index) >=
        static_cast<int>(size)) {
      return false;
    }
    struct stat info = {};
    if (stat(buffer, &info) != 0) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Fades the white shutter flash back out.
 * @param object Flash overlay.
 * @param value Opacity for this animation step.
 */
void SetShutterFlashOpacity(void* object, int32_t value) {
  lv_obj_set_style_bg_opa(static_cast<lv_obj_t*>(object),
      static_cast<lv_opa_t>(value), LV_PART_MAIN);
}

/**
 * @brief Clears the capture toast once it has been read.
 * @param timer LVGL timer.
 */
void HideCaptureToastCallback(lv_timer_t* timer) {
  auto* state = static_cast<CameraViewState*>(lv_timer_get_user_data(timer));
  if (state != nullptr) {
    state->toast_timer = nullptr;
    if (state->capture_toast != nullptr) {
      lv_obj_add_flag(state->capture_toast, LV_OBJ_FLAG_HIDDEN);
    }
  }
  lv_timer_delete(timer);
}

/**
 * @brief Shows a short message over the preview.
 * @param state Camera view state.
 * @param text Message to show.
 */
void ShowCaptureToast(CameraViewState* state, const char* text) {
  if (state->capture_toast == nullptr) {
    return;
  }
  lv_label_set_text(state->capture_toast, text);
  lv_obj_remove_flag(state->capture_toast, LV_OBJ_FLAG_HIDDEN);
  if (state->toast_timer != nullptr) {
    lv_timer_reset(state->toast_timer);
    return;
  }
  state->toast_timer =
      lv_timer_create(HideCaptureToastCallback, kCaptureToastMs, state);
}

/**
 * @brief Builds the path for a new video from the RTC clock.
 * @param state Camera view state.
 * @param buffer Output buffer.
 * @param size Buffer size.
 * @return true when a path could be built.
 */
bool BuildVideoPath(CameraViewState* state, char* buffer, size_t size) {
  char photo[160] = {};
  bool on_card = false;
  // Video fills the 4 MB of internal memory in seconds; it needs the card.
  if (!BuildPhotoPath(state, photo, sizeof(photo), &on_card) || !on_card) {
    return false;
  }
  // Same naming as photos, only the extension differs, so the two sort
  // together in the library.
  const size_t length = std::strlen(photo);
  if (length < 4 || length >= size) {
    return false;
  }
  std::snprintf(buffer, size, "%.*s.avi", static_cast<int>(length - 4), photo);
  // VID_ reads better than IMG_ for a video.
  char* name = std::strrchr(buffer, '/');
  if (name != nullptr && std::strncmp(name + 1, "IMG_", 4) == 0) {
    std::memcpy(name + 1, "VID_", 4);
  }
  return true;
}

/**
 * @brief Repaints the shutter for the current mode and recording state.
 * @param state Camera view state.
 */
void ApplyShutterState(CameraViewState* state) {
  if (state->shutter_inner == nullptr) {
    return;
  }
  const bool video = state->video_mode;
  const int size = state->recording ? 44 : (video ? 84 : kShutterButtonSize - 20);
  lv_obj_set_size(state->shutter_inner, size, size);
  lv_obj_set_style_radius(state->shutter_inner,
      state->recording ? 10 : LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->shutter_inner,
      lv_color_hex(video ? 0xE5484D : 0xFFFFFF), LV_PART_MAIN);
  lv_obj_center(state->shutter_inner);

  if (state->mode_label != nullptr) {
    lv_label_set_text(state->mode_label,
        video ? icon::kPhotoCamera : icon::kVideocam);
  }
  if (state->recording_label != nullptr) {
    if (state->recording) {
      lv_obj_remove_flag(state->recording_label, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(state->recording_label, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

/**
 * @brief Keeps the recording time and size on screen.
 * @param timer LVGL timer.
 */
void RecordingTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<CameraViewState*>(lv_timer_get_user_data(timer));
  if (state == nullptr || state->camera == nullptr) {
    return;
  }
  hal::VideoRecordingStatus status;
  if (!state->camera->ReadVideoRecordingStatus(&status)) {
    return;
  }

  if (state->recording && state->recording_label != nullptr) {
    const unsigned int seconds = status.elapsed_ms / 1000U;
    char text[64] = {};
    std::snprintf(text, sizeof(text), "%s  %u:%02u   %u fps   %llu MB",
        icon::kFiberManualRecord, seconds / 60U, seconds % 60U,
        static_cast<unsigned int>(status.frames_per_second),
        static_cast<unsigned long long>(status.bytes_written / 1000000ULL));
    lv_label_set_text(state->recording_label, text);
  }

  // The task stops on its own when the card fills up or the file hits its
  // limit, so the button has to follow rather than lead.
  if (state->recording &&
      status.state != hal::VideoRecordingState::kRecording) {
    state->recording = false;
    ApplyShutterState(state);
    ShowCaptureToast(state,
        status.state == hal::VideoRecordingState::kError ? "Recording failed"
                                                         : "Video saved");
    PlayUiHapticEffect(status.state == hal::VideoRecordingState::kError
            ? HapticEffect::kWarning
            : HapticEffect::kSuccess);
  }
}

/**
 * @brief Switches between photo and video.
 * @param event LVGL event.
 */
void ModeClickedEventCallback(lv_event_t* event) {
  auto* state = static_cast<CameraViewState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->recording) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  state->video_mode = !state->video_mode;
  ApplyShutterState(state);
  ShowCaptureToast(state, state->video_mode ? "Video" : "Photo");
}

/**
 * @brief Steps the preview through its zoom levels, wide to close and round.
 *
 * The sensor is landscape and the screen portrait, so the closer steps trade
 * field of view for a bigger viewfinder. Not while recording: the video's
 * frame size is fixed when it starts.
 * @param event LVGL event.
 */
void ZoomClickedEventCallback(lv_event_t* event) {
  auto* state = static_cast<CameraViewState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->camera == nullptr || state->recording) {
    return;
  }
  const int count = state->camera->CameraPreviewZoomStepCount();
  if (count < 2) {
    return;
  }
  const int next = (state->camera->GetCameraPreviewZoomStep() + 1) % count;
  if (!state->camera->SetCameraPreviewZoomStep(next)) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  if (state->zoom_label != nullptr) {
    lv_label_set_text(state->zoom_label,
        state->camera->CameraPreviewZoomLabel(next));
  }
}

/**
 * @brief Captures a photo when the shutter is tapped.
 * @param event LVGL event.
 */
void ShutterClickedEventCallback(lv_event_t* event) {
  auto* state = static_cast<CameraViewState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->camera == nullptr) {
    return;
  }
  if (state->content_state != CameraContentState::kPreview) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }

  if (state->video_mode) {
    if (state->recording) {
      PlayUiHapticEffect(HapticEffect::kImpactMedium);
      state->camera->StopVideoRecording();
      return;
    }
    char path[160] = {};
    if (!BuildVideoPath(state, path, sizeof(path))) {
      PlayUiHapticEffect(HapticEffect::kWarning);
      ShowCaptureToast(state, "Video needs an SD card");
      return;
    }
    if (!state->camera->StartVideoRecording(path)) {
      PlayUiHapticEffect(HapticEffect::kWarning);
      ShowCaptureToast(state, "Could not start recording");
      return;
    }
    PlayUiHapticEffect(HapticEffect::kImpactHeavy);
    state->recording = true;
    ApplyShutterState(state);
    return;
  }

  char path[160] = {};
  if (!BuildPhotoPath(state, path, sizeof(path))) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    ShowCaptureToast(state, "Phone memory full - insert an SD card");
    return;
  }

  PlayUiHapticEffect(HapticEffect::kImpactHeavy);
  if (!state->camera->CaptureCameraPhoto(path)) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    ShowCaptureToast(state, "Capture failed");
    return;
  }

  // A quick white flash is the whole feedback loop for a camera shutter.
  if (state->shutter_flash != nullptr) {
    lv_anim_delete(state->shutter_flash, SetShutterFlashOpacity);
    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, state->shutter_flash);
    lv_anim_set_values(&animation, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&animation, kShutterFlashMs);
    lv_anim_set_exec_cb(&animation, SetShutterFlashOpacity);
    lv_anim_start(&animation);
  }
  const char* name = std::strrchr(path, '/');
  ShowCaptureToast(state, name == nullptr ? path : name + 1);
}

void MakeTransparent(lv_obj_t* object) {
  if (object == nullptr) {
    return;
  }
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
}

void RetryCameraClickedEventCallback(lv_event_t* event);

/**
 * @brief 定位摄像头页面状态提示
 * @param state 摄像头页面状态
 * @param group 状态提示容器
 */
void PositionCameraPromptStatus(CameraViewState* state, lv_obj_t* group) {
  if (state == nullptr || group == nullptr) {
    return;
  }
  lv_display_t* display = lv_obj_get_display(group);
  const int display_height = display == nullptr
                                 ? state->height
                                 : lv_display_get_vertical_resolution(display);
  const int height = lv_obj_get_height(group);
  const int centered_top =
      (display_height - height) / 2 + kCameraPromptGroupOffsetY;
  const int maximum_top = std::max(0, display_height - height);
  const int group_top = std::min(std::max(centered_top, 0), maximum_top);
  lv_obj_align(group, LV_ALIGN_TOP_MID, 0, group_top);
}

/**
 * @brief 设置摄像头页面的黑色背景和浅色状态栏
 * @param state 摄像头页面状态
 */
void ApplyCameraPageColors(CameraViewState* state) {
  if (state == nullptr || state->container == nullptr) {
    return;
  }
  lv_obj_set_style_bg_color(state->container,
      lv_color_hex(theme::FixedColors().camera_background), LV_PART_MAIN);
  if (state->set_status_bar_text_color) {
    state->set_status_bar_text_color(theme::FixedColors().camera_primary_text);
  }
}

/**
 * @brief 渲染摄像头扫描中的加载状态
 * @param state 摄像头页面状态
 */
void RenderCameraScanning(CameraViewState* state) {
  if (state == nullptr || state->status_layer == nullptr) {
    return;
  }
  state->content_state = CameraContentState::kScanning;
  state->displayed_error = CameraError::kNone;
  ApplyCameraPageColors(state);
  if (state->image != nullptr) {
    lv_obj_add_flag(state->image, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_remove_flag(state->status_layer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(state->status_layer);

  PromptStatusConfig config;
  config.width = state->width;
  config.height = kCameraPromptGroupHeight;
  config.visual = PromptStatusVisual::kSpinner;
  config.spinner_track_color =
      theme::ActiveThemeColors().surface_container_high;
  config.spinner_indicator_color = theme::FixedColors().action;
  config.title = "Looking for a camera...";
  config.title_font = Font28();
  config.title_color = theme::FixedColors().camera_primary_text;
  config.title_top = 96;
  config.message = "Keep the camera connected while scanning";
  config.message_font = Font22();
  config.message_color = theme::FixedColors().camera_secondary_text;
  config.message_top = 138;
  lv_obj_t* group = CreatePromptStatus(state->status_layer, config);
  PositionCameraPromptStatus(state, group);
}

/**
 * @brief 渲染摄像头扫描或初始化失败状态
 * @param state 摄像头页面状态
 * @param error 摄像头错误
 */
void RenderCameraError(CameraViewState* state, CameraError error) {
  if (state == nullptr || state->status_layer == nullptr) {
    return;
  }
  if (state->content_state == CameraContentState::kError &&
      state->displayed_error == error) {
    return;
  }

  const DiagnosticError diagnostic_error = GetCameraDiagnosticError(error);
  LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
      "StartCameraPreview failed: %s: %s\n", diagnostic_error.code,
      diagnostic_error.text);
  state->content_state = CameraContentState::kError;
  state->displayed_error = error;
  state->preview_started = false;
  ApplyCameraPageColors(state);
  if (state->image != nullptr) {
    lv_obj_add_flag(state->image, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_remove_flag(state->status_layer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clean(state->status_layer);

  char hint_text[128] = {};
  std::snprintf(hint_text, sizeof(hint_text), "%s: %s", diagnostic_error.code,
      diagnostic_error.text);
  PromptStatusConfig config;
  config.width = state->width;
  config.height = kCameraPromptGroupHeight;
  config.icon = icon::kCamera;
  config.icon_font = &lvgl_font_material_symbols_fill_56;
  config.title = "Camera unavailable";
  config.title_font = Font28();
  config.title_color = theme::FixedColors().camera_primary_text;
  config.message = hint_text;
  config.message_font = Font22();
  config.message_color = theme::FixedColors().camera_secondary_text;
  config.button_text = "Retry";
  config.button_font = &lvgl_font_google_sans_flex_24;
  config.button_callback = RetryCameraClickedEventCallback;
  config.button_user_data = state;
  lv_obj_t* group = CreatePromptStatus(state->status_layer, config);
  PositionCameraPromptStatus(state, group);
}

/**
 * @brief 切换为摄像头预览状态
 * @param state 摄像头页面状态
 */
void RenderCameraPreview(CameraViewState* state) {
  if (state == nullptr) {
    return;
  }
  state->content_state = CameraContentState::kPreview;
  state->displayed_error = CameraError::kNone;
  ApplyCameraPageColors(state);
  if (state->status_layer != nullptr) {
    lv_obj_add_flag(state->status_layer, LV_OBJ_FLAG_HIDDEN);
  }
  if (state->image != nullptr) {
    lv_obj_remove_flag(state->image, LV_OBJ_FLAG_HIDDEN);
  }
}

/**
 * @brief 清除上一次预览的帧元数据
 * @param state 摄像头页面状态
 */
void ResetCameraFrameState(CameraViewState* state) {
  if (state == nullptr) {
    return;
  }
  state->last_sequence = 0;
  state->last_width = 0;
  state->last_height = 0;
  state->last_stride = 0;
  state->last_bits_per_pixel = 0;
  state->image_dsc = {};
}

/**
 * @brief 提交后台摄像头扫描请求
 * @param state 摄像头页面状态
 * @return 请求成功提交返回 true，否则返回 false
 */
bool StartCameraScan(CameraViewState* state) {
  if (state == nullptr) {
    return false;
  }
  if (state->camera == nullptr) {
    RenderCameraError(state, CameraError::kProviderUnavailable);
    return false;
  }
  if (!state->camera->StartCameraPreview()) {
    CameraError error = state->camera->GetCameraPreviewError();
    if (error == CameraError::kNone) {
      error = CameraError::kVideoInitFailed;
    }
    RenderCameraError(state, error);
    return false;
  }
  state->preview_started = true;
  return true;
}

/**
 * @brief 处理重新扫描按钮点击事件
 * @param event LVGL 事件对象
 */
void RetryCameraClickedEventCallback(lv_event_t* event) {
  if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
    return;
  }
  auto* state = static_cast<CameraViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }

  // 错误发布与后台任务清理之间可能存在极短窗口，先等待旧任务退出再重启。
  if (state->camera != nullptr) {
    state->camera->StopCameraPreview();
  }
  ResetCameraFrameState(state);
  RenderCameraScanning(state);
  StartCameraScan(state);
}

/**
 * @brief 根据像素位宽获取 LVGL 颜色格式
 * @param bits_per_pixel 像素位宽
 * @return LVGL 颜色格式
 */
lv_color_format_t ColorFormatForBitsPerPixel(uint32_t bits_per_pixel) {
  return bits_per_pixel == 24 ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_RGB565;
}

/**
 * @brief 确保 LVGL 预览缓冲区可用
 * @param state 摄像头页面状态
 * @param required_size 需要的缓冲区大小
 * @return 缓冲区可用返回 true，否则返回 false
 */
bool EnsureFrameBuffer(CameraViewState* state, size_t required_size) {
  if (state == nullptr || required_size == 0) {
    return false;
  }
  if (state->frame_buffer != nullptr &&
      state->frame_buffer_size >= required_size) {
    return true;
  }

  if (state->frame_buffer != nullptr) {
    heap_caps_free(state->frame_buffer);
    state->frame_buffer = nullptr;
    state->frame_buffer_size = 0;
  }
  state->frame_buffer =
      static_cast<uint8_t*>(heap_caps_malloc(required_size, MALLOC_CAP_SPIRAM));
  if (state->frame_buffer == nullptr) {
    return false;
  }
  state->frame_buffer_size = required_size;
  return true;
}

/**
 * @brief 根据锁屏状态停止或恢复摄像头预览
 * @param state 摄像头页面状态
 * @param visible true 表示锁屏显示，false 表示锁屏隐藏
 */
void SetCameraLockScreenPaused(CameraViewState* state, bool visible) {
  if (state == nullptr || state->camera == nullptr ||
      state->lock_screen_paused == visible) {
    return;
  }

  state->lock_screen_paused = visible;
  if (!state->preview_started) {
    return;
  }
  if (visible) {
    state->camera->StopCameraPreview();
    return;
  }

  ResetCameraFrameState(state);
  RenderCameraScanning(state);
  StartCameraScan(state);
}

/**
 * @brief 锁屏显示状态变化回调
 * @param state 摄像头页面状态
 * @param visible true 表示锁屏显示，false 表示锁屏隐藏
 */
void CameraLockScreenVisibilityCallback(CameraViewState* state, bool visible) {
  SetCameraLockScreenPaused(state, visible);
}

/**
 * @brief 刷新摄像头页面状态和预览帧
 * @param timer LVGL 定时器
 */
void CameraRefreshTimerCallback(lv_timer_t* timer) {
  if (timer == nullptr) {
    return;
  }

  auto* state = static_cast<CameraViewState*>(lv_timer_get_user_data(timer));
  if (state == nullptr || state->camera == nullptr || state->image == nullptr ||
      state->lock_screen_paused ||
      state->content_state == CameraContentState::kError) {
    return;
  }

  hal::CameraPreviewFrameInfo info;
  if (!state->camera->GetCameraPreviewFrameInfo(&info)) {
    const CameraError error = state->camera->GetCameraPreviewError();
    if (error != CameraError::kNone) {
      RenderCameraError(state, error);
    }
    return;
  }
  if (info.sequence == state->last_sequence) {
    return;
  }
  if (!EnsureFrameBuffer(state, info.data_size)) {
    state->camera->StopCameraPreview();
    RenderCameraError(state, CameraError::kOutputBufferAllocationFailed);
    return;
  }
  if (!state->camera->CopyCameraPreviewFrame(
          state->frame_buffer, state->frame_buffer_size, &info)) {
    return;
  }

  state->last_sequence = info.sequence;
  const bool source_changed = state->image_dsc.data != state->frame_buffer ||
                              state->last_width != info.width ||
                              state->last_height != info.height ||
                              state->last_stride != info.stride ||
                              state->last_bits_per_pixel != info.bits_per_pixel;
  state->image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  state->image_dsc.header.cf = ColorFormatForBitsPerPixel(info.bits_per_pixel);
  state->image_dsc.header.flags = 0;
  state->image_dsc.header.w = info.width;
  state->image_dsc.header.h = info.height;
  state->image_dsc.header.stride = info.stride;
  state->image_dsc.data_size = info.data_size;
  state->image_dsc.data = state->frame_buffer;
  if (source_changed) {
    lv_image_set_src(state->image, &state->image_dsc);
    if (static_cast<int>(info.height) < state->height) {
      // A viewfinder shorter than the screen sits in the upper part, leaving
      // the space below it to the shutter and mode buttons, rather than
      // floating in the middle between two equal bars.
      lv_obj_align(state->image, LV_ALIGN_TOP_MID, 0,
          (state->height - static_cast<int>(info.height)) / 3);
    } else {
      lv_obj_center(state->image);
    }
    state->last_width = info.width;
    state->last_height = info.height;
    state->last_stride = info.stride;
    state->last_bits_per_pixel = info.bits_per_pixel;
  }
  if (state->content_state != CameraContentState::kPreview) {
    RenderCameraPreview(state);
  }
  lv_obj_invalidate(state->image);
}

/**
 * @brief 摄像头页面删除时停止预览并释放状态
 * @param event LVGL 事件对象
 */
void CameraViewDeleteEventCallback(lv_event_t* event) {
  if (lv_event_get_code(event) != LV_EVENT_DELETE) {
    return;
  }

  auto* state = static_cast<CameraViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }

  SetEdgeSwipePassthroughMode(false);

  // A recording task outliving its view would keep writing to a file nobody
  // is watching, so it is stopped before anything else is torn down.
  if (state->recording && state->camera != nullptr) {
    state->camera->StopVideoRecording();
    state->recording = false;
  }
  if (state->recording_timer != nullptr) {
    lv_timer_delete(state->recording_timer);
    state->recording_timer = nullptr;
  }
  if (state->toast_timer != nullptr) {
    lv_timer_delete(state->toast_timer);
    state->toast_timer = nullptr;
  }
  if (state->refresh_timer != nullptr) {
    lv_timer_delete(state->refresh_timer);
    state->refresh_timer = nullptr;
  }
  if (state->set_lock_screen_visibility_callback) {
    state->set_lock_screen_visibility_callback(nullptr);
  }
  if (state->camera != nullptr) {
    if (state->dismiss_stop_requested) {
      state->camera->RequestCameraPreviewStop();
    } else {
      state->camera->StopCameraPreview();
    }
  }
  if (state->frame_buffer != nullptr) {
    heap_caps_free(state->frame_buffer);
    state->frame_buffer = nullptr;
    state->frame_buffer_size = 0;
  }
  delete state;
}

}  // namespace

lv_obj_t* CreateCameraView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config) {
  (void)app_entry;
  if (parent == nullptr || config.width <= 0 || config.height <= 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "CreateCameraView received invalid input (parent: %p, width: %d, "
        "height: %d)\n",
        parent, config.width, config.height);
    return nullptr;
  }
  lv_obj_t* container = lv_obj_create(parent);
  if (container == nullptr) {
    return nullptr;
  }

  lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(container, config.width, config.height);
  lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(container,
      lv_color_hex(theme::FixedColors().camera_background), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);

  if (config.set_status_bar_visible) {
    config.set_status_bar_visible(true);
  }

  auto* state = new (std::nothrow) CameraViewState();
  if (state == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  state->camera = config.camera;
  state->storage = config.storage;
  state->rtc = config.rtc;
  state->container = container;
  state->width = config.width;
  state->height = config.height;
  state->set_status_bar_text_color = config.set_status_bar_text_color;
  state->set_lock_screen_visibility_callback =
      config.set_lock_screen_visibility_callback;
  lv_obj_set_user_data(container, state);
  lv_obj_add_event_cb(
      container, CameraViewDeleteEventCallback, LV_EVENT_DELETE, state);

  state->image = lv_image_create(container);
  if (state->image == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  lv_obj_remove_flag(state->image, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(state->image, LV_OBJ_FLAG_HIDDEN);
  lv_obj_center(state->image);

  state->status_layer = lv_obj_create(container);
  if (state->status_layer == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  MakeTransparent(state->status_layer);
  lv_obj_remove_flag(state->status_layer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(state->status_layer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(state->status_layer, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_size(state->status_layer, config.width, config.height);
  lv_obj_align(state->status_layer, LV_ALIGN_CENTER, 0, 0);

  // Shutter furniture sits above the preview but below the status layer so a
  // camera error message still wins the screen.
  state->shutter_flash = lv_obj_create(container);
  if (state->shutter_flash != nullptr) {
    lv_obj_remove_flag(state->shutter_flash, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state->shutter_flash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(state->shutter_flash, config.width, config.height);
    lv_obj_set_pos(state->shutter_flash, 0, 0);
    lv_obj_set_style_radius(state->shutter_flash, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->shutter_flash, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->shutter_flash, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->shutter_flash, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->shutter_flash, LV_OPA_TRANSP, LV_PART_MAIN);
  }

  state->capture_toast = lv_label_create(container);
  if (state->capture_toast != nullptr) {
    lv_obj_add_flag(state->capture_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(
        state->capture_toast, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        state->capture_toast, &lvgl_font_google_sans_flex_24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->capture_toast, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->capture_toast, 150, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->capture_toast, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(state->capture_toast, 16, LV_PART_MAIN);
    lv_obj_align(state->capture_toast, LV_ALIGN_BOTTOM_MID, 0,
        -kShutterBottomMargin - kShutterButtonSize - 26);
  }

  state->shutter_button = lv_button_create(container);
  if (state->shutter_button != nullptr) {
    lv_obj_set_size(
        state->shutter_button, kShutterButtonSize, kShutterButtonSize);
    lv_obj_align(
        state->shutter_button, LV_ALIGN_BOTTOM_MID, 0, -kShutterBottomMargin);
    lv_obj_set_style_radius(
        state->shutter_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->shutter_button, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->shutter_button, 235, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->shutter_button, 6, LV_PART_MAIN);
    lv_obj_set_style_border_color(
        state->shutter_button, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_border_opa(state->shutter_button, 120, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->shutter_button, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->shutter_button, 60, LV_PART_MAIN);
    lv_obj_add_event_cb(state->shutter_button, ShutterClickedEventCallback,
        LV_EVENT_CLICKED, state);

    state->shutter_inner = lv_obj_create(state->shutter_button);
    if (state->shutter_inner != nullptr) {
      lv_obj_remove_flag(state->shutter_inner, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(state->shutter_inner, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_border_width(state->shutter_inner, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(state->shutter_inner, 0, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(state->shutter_inner, LV_OPA_COVER,
          LV_PART_MAIN);
    }
  }

  state->mode_button = lv_button_create(container);
  if (state->mode_button != nullptr) {
    lv_obj_set_size(state->mode_button, kModeButtonSize, kModeButtonSize);
    lv_obj_align(state->mode_button, LV_ALIGN_BOTTOM_RIGHT, -46,
        -kShutterBottomMargin - 18);
    lv_obj_set_style_radius(
        state->mode_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->mode_button, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->mode_button, 120, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->mode_button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(state->mode_button, ModeClickedEventCallback,
        LV_EVENT_CLICKED, state);
    state->mode_label = lv_label_create(state->mode_button);
    if (state->mode_label != nullptr) {
      lv_label_set_text(state->mode_label, icon::kVideocam);
      lv_obj_set_style_text_color(
          state->mode_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
      lv_obj_set_style_text_font(state->mode_label,
          &lvgl_font_material_symbols_fill_32, LV_PART_MAIN);
      lv_obj_center(state->mode_label);
    }
  }

  if (state->camera != nullptr &&
      state->camera->CameraPreviewZoomStepCount() > 1) {
    state->zoom_button = lv_button_create(container);
  }
  if (state->zoom_button != nullptr) {
    // Mirrors the mode button on the other side of the shutter.
    lv_obj_set_size(state->zoom_button, kModeButtonSize, kModeButtonSize);
    lv_obj_align(state->zoom_button, LV_ALIGN_BOTTOM_LEFT, 46,
        -kShutterBottomMargin - 18);
    lv_obj_set_style_radius(
        state->zoom_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->zoom_button, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->zoom_button, 120, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->zoom_button, 0, LV_PART_MAIN);
    lv_obj_add_event_cb(state->zoom_button, ZoomClickedEventCallback,
        LV_EVENT_CLICKED, state);
    state->zoom_label = lv_label_create(state->zoom_button);
    if (state->zoom_label != nullptr) {
      lv_label_set_text(state->zoom_label,
          state->camera->CameraPreviewZoomLabel(
              state->camera->GetCameraPreviewZoomStep()));
      lv_obj_set_style_text_color(
          state->zoom_label, lv_color_hex(0xFFD60A), LV_PART_MAIN);
      lv_obj_set_style_text_font(state->zoom_label, Font22(), LV_PART_MAIN);
      lv_obj_center(state->zoom_label);
    }
  }

  state->recording_label = lv_label_create(container);
  if (state->recording_label != nullptr) {
    lv_obj_add_flag(state->recording_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_color(
        state->recording_label, lv_color_hex(0xFF5A5A), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        state->recording_label, &lvgl_font_google_sans_flex_24, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->recording_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->recording_label, 140, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->recording_label, 10, LV_PART_MAIN);
    lv_obj_set_style_radius(state->recording_label, 14, LV_PART_MAIN);
    lv_obj_align(state->recording_label, LV_ALIGN_TOP_MID, 0, 96);
  }
  ApplyShutterState(state);

  state->recording_timer =
      lv_timer_create(RecordingTimerCallback, kRecordingTimerMs, state);

  if (state->set_lock_screen_visibility_callback) {
    state->set_lock_screen_visibility_callback([state](bool visible) {
      CameraLockScreenVisibilityCallback(state, visible);
    });
  }

  state->refresh_timer = lv_timer_create(
      CameraRefreshTimerCallback, kCameraViewRefreshPeriodMs, state);
  if (state->refresh_timer == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }

  SetEdgeSwipePassthroughMode(true);
  RenderCameraScanning(state);
  StartCameraScan(state);
  CameraRefreshTimerCallback(state->refresh_timer);
  return container;
}

void PrepareCameraViewForDismiss(lv_obj_t* camera_view) {
  if (camera_view == nullptr) {
    return;
  }
  auto* state =
      static_cast<CameraViewState*>(lv_obj_get_user_data(camera_view));
  if (state == nullptr || state->container != camera_view) {
    return;
  }
  state->dismiss_stop_requested = true;
  if (state->camera != nullptr) {
    state->camera->RequestCameraPreviewStop();
  }
}

}  // namespace lilygo_box::ui
