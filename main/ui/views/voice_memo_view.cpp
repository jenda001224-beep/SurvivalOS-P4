/*
 * @Description: Voice memo app view
 * @License: GPL 3.0
 */
#include "ui/views/voice_memo_view.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "audio/wav_decoder.h"
#include "base/logger.h"
#include "hal/providers/audio_provider.h"
#include "hal/providers/rtc_provider.h"
#include "hal/providers/storage_provider.h"
#include "ui/haptic_feedback.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"

namespace lilygo_box::ui {
namespace {

constexpr const char* kRecordingsDirectory = "voice";
constexpr size_t kMaxRecordings = 64;
constexpr size_t kMaxPathLength = 128;
constexpr uint32_t kRefreshIntervalMs = 100;

constexpr int kHeaderHeight = 108;
constexpr int kRowHeight = 96;
constexpr int kRowGap = 8;
constexpr int kSidePadding = 20;
constexpr int kTransportHeight = 240;
constexpr int kRecordButtonSize = 132;
constexpr int kRecordInnerIdleSize = 96;
constexpr int kRecordInnerActiveSize = 56;

constexpr uint32_t kRecordColor = 0xE5484D;
constexpr uint32_t kMeterColor = 0xE5484D;

// One entry in the recordings list.
struct Recording {
  char name[64] = {};
  char path[kMaxPathLength] = {};
  uint32_t duration_ms = 0;
};

struct VoiceMemoViewState {
  hal::AudioProvider* audio = nullptr;
  hal::StorageProvider* storage = nullptr;
  hal::RtcProvider* rtc = nullptr;

  lv_obj_t* root = nullptr;
  lv_obj_t* list = nullptr;
  lv_obj_t* empty_label = nullptr;
  lv_obj_t* record_button = nullptr;
  lv_obj_t* record_inner = nullptr;
  lv_obj_t* status_label = nullptr;
  lv_obj_t* meter = nullptr;
  lv_timer_t* timer = nullptr;

  Recording recordings[kMaxRecordings];
  size_t recording_count = 0;
  int playing_index = -1;
  bool recording = false;
  char pending_path[kMaxPathLength] = {};
};

/**
 * @brief Strips background, border and padding from an object.
 * @param object Object to flatten.
 */
void MakeTransparent(lv_obj_t* object) {
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
}

/**
 * @brief Creates a styled label.
 * @param parent Parent object.
 * @param text Initial text.
 * @param color Text colour in 0xRRGGBB form.
 * @param font Font to use.
 * @return The label, or nullptr on failure.
 */
lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, uint32_t color,
    const lv_font_t* font) {
  lv_obj_t* label = lv_label_create(parent);
  if (label == nullptr) {
    return nullptr;
  }
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  return label;
}

/**
 * @brief Writes a duration as m:ss into a buffer.
 * @param buffer Output buffer.
 * @param size Buffer size.
 * @param milliseconds Duration in milliseconds.
 */
void FormatDuration(char* buffer, size_t size, uint32_t milliseconds) {
  const unsigned int seconds = static_cast<unsigned int>(milliseconds / 1000U);
  std::snprintf(buffer, size, "%u:%02u", seconds / 60U, seconds % 60U);
}

/**
 * @brief Builds the directory the recordings live in.
 * @param state View state.
 * @param buffer Output buffer.
 * @param size Buffer size.
 * @return true when the SD card is mounted and the path was built.
 */
bool BuildDirectoryPath(
    const VoiceMemoViewState* state, char* buffer, size_t size) {
  if (state->storage == nullptr || !state->storage->IsSdCardMounted()) {
    return false;
  }
  const char* base = state->storage->SdCardBasePath();
  if (base == nullptr || base[0] == '\0') {
    return false;
  }
  return std::snprintf(buffer, size, "%s/%s", base, kRecordingsDirectory) <
      static_cast<int>(size);
}

/**
 * @brief Sorts recordings newest first, which is what the filenames encode.
 * @param state View state.
 */
void SortRecordings(VoiceMemoViewState* state) {
  std::sort(state->recordings, state->recordings + state->recording_count,
      [](const Recording& left, const Recording& right) {
        return std::strcmp(left.name, right.name) > 0;
      });
}

/**
 * @brief Rescans the recordings directory.
 * @param state View state.
 */
void ScanRecordings(VoiceMemoViewState* state) {
  state->recording_count = 0;
  char directory[kMaxPathLength] = {};
  if (!BuildDirectoryPath(state, directory, sizeof(directory))) {
    return;
  }

  DIR* handle = opendir(directory);
  if (handle == nullptr) {
    // The folder simply does not exist yet; it is created on first recording.
    return;
  }
  while (state->recording_count < kMaxRecordings) {
    const dirent* entry = readdir(handle);
    if (entry == nullptr) {
      break;
    }
    const size_t length = std::strlen(entry->d_name);
    if (length < 5 || strcasecmp(entry->d_name + length - 4, ".wav") != 0) {
      continue;
    }
    Recording& recording = state->recordings[state->recording_count];
    std::snprintf(recording.name, sizeof(recording.name), "%s", entry->d_name);
    std::snprintf(recording.path, sizeof(recording.path), "%s/%s", directory,
        entry->d_name);
    audio::WavStreamInfo info;
    recording.duration_ms =
        audio::ReadWavStreamInfo(recording.path, &info) ? info.duration_ms : 0;
    ++state->recording_count;
  }
  closedir(handle);
  SortRecordings(state);
}

/**
 * @brief Stops any playback that is running.
 * @param state View state.
 */
void StopPlayback(VoiceMemoViewState* state) {
  if (state->playing_index >= 0 && state->audio != nullptr) {
    state->audio->StopAudioFile();
  }
  state->playing_index = -1;
}

void RebuildList(VoiceMemoViewState* state);

/**
 * @brief Starts or stops playing the tapped recording.
 * @param event LVGL event.
 */
void RecordingClickedCallback(lv_event_t* event) {
  auto* state =
      static_cast<VoiceMemoViewState*>(lv_event_get_user_data(event));
  lv_obj_t* row = lv_event_get_target_obj(event);
  if (state == nullptr || row == nullptr || state->audio == nullptr) {
    return;
  }
  const int index = static_cast<int>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
  if (index < 0 || index >= static_cast<int>(state->recording_count)) {
    return;
  }

  PlayUiHapticEffect(HapticEffect::kImpactLight);
  if (state->playing_index == index) {
    StopPlayback(state);
    RebuildList(state);
    return;
  }

  StopPlayback(state);
  const Recording& recording = state->recordings[index];
  if (state->audio->StartAudioFile(recording.path, recording.duration_ms)) {
    state->playing_index = index;
  } else {
    PlayUiHapticEffect(HapticEffect::kWarning);
  }
  RebuildList(state);
}

/**
 * @brief Deletes the recording whose delete button was tapped.
 * @param event LVGL event.
 */
void DeleteClickedCallback(lv_event_t* event) {
  auto* state =
      static_cast<VoiceMemoViewState*>(lv_event_get_user_data(event));
  lv_obj_t* button = lv_event_get_target_obj(event);
  if (state == nullptr || button == nullptr) {
    return;
  }
  const int index = static_cast<int>(
      reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
  if (index < 0 || index >= static_cast<int>(state->recording_count)) {
    return;
  }

  if (state->playing_index == index) {
    StopPlayback(state);
  }
  if (std::remove(state->recordings[index].path) != 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Delete recording failed: %s\n", state->recordings[index].path);
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }
  PlayUiHapticEffect(HapticEffect::kSuccess);
  state->playing_index = -1;
  ScanRecordings(state);
  RebuildList(state);
}

/**
 * @brief Rebuilds the list rows from the current recordings.
 * @param state View state.
 */
void RebuildList(VoiceMemoViewState* state) {
  if (state->list == nullptr) {
    return;
  }
  lv_obj_clean(state->list);

  if (state->recording_count == 0) {
    lv_obj_t* empty = CreateLabel(state->list,
        "No recordings yet.\nTap the button below to start.", 0x8A8A8E,
        &lvgl_font_google_sans_flex_24);
    if (empty != nullptr) {
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 60);
    }
    return;
  }

  const int list_width = lv_obj_get_width(state->list);
  for (size_t index = 0; index < state->recording_count; ++index) {
    const Recording& recording = state->recordings[index];
    lv_obj_t* row = lv_obj_create(state->list);
    if (row == nullptr) {
      return;
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, list_width, kRowHeight);
    lv_obj_set_pos(row, 0, static_cast<int>(index) * (kRowHeight + kRowGap));
    lv_obj_set_style_radius(row, 22, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row,
        lv_color_hex(theme::ActiveThemeColors().surface_container_high),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_user_data(row, reinterpret_cast<void*>(
        static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(row, RecordingClickedCallback, LV_EVENT_CLICKED, state);

    lv_obj_t* glyph = CreateLabel(row,
        state->playing_index == static_cast<int>(index) ? icon::kPause
                                                        : icon::kPlayArrow,
        theme::ActiveThemeColors().on_surface,
        &lvgl_font_material_symbols_fill_32);
    if (glyph != nullptr) {
      lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 22, 0);
    }

    lv_obj_t* name = CreateLabel(row, recording.name,
        theme::ActiveThemeColors().on_surface,
        &lvgl_font_google_sans_flex_24);
    if (name != nullptr) {
      lv_obj_align(name, LV_ALIGN_LEFT_MID, 74, -14);
    }

    char duration[16] = {};
    FormatDuration(duration, sizeof(duration), recording.duration_ms);
    lv_obj_t* meta = CreateLabel(row, duration,
        theme::ActiveThemeColors().on_surface_variant,
        &lvgl_font_google_sans_flex_22);
    if (meta != nullptr) {
      lv_obj_align(meta, LV_ALIGN_LEFT_MID, 74, 18);
    }

    lv_obj_t* remove = lv_button_create(row);
    if (remove != nullptr) {
      lv_obj_set_size(remove, 64, 64);
      lv_obj_align(remove, LV_ALIGN_RIGHT_MID, -14, 0);
      lv_obj_set_style_radius(remove, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(remove,
          lv_color_hex(theme::ActiveThemeColors().surface_container_highest),
          LV_PART_MAIN);
      lv_obj_set_user_data(remove, reinterpret_cast<void*>(
          static_cast<intptr_t>(index)));
      lv_obj_add_event_cb(remove, DeleteClickedCallback, LV_EVENT_CLICKED,
          state);
      lv_obj_t* remove_glyph = CreateLabel(remove, icon::kDelete,
          theme::ActiveThemeColors().on_surface_variant,
          &lvgl_font_material_symbols_fill_32);
      if (remove_glyph != nullptr) {
        lv_obj_center(remove_glyph);
      }
    }
  }
}

/**
 * @brief Builds the path for a new recording from the RTC clock.
 * @param state View state.
 * @param buffer Output buffer.
 * @param size Buffer size.
 * @return true when a path could be built.
 */
bool BuildNewRecordingPath(
    VoiceMemoViewState* state, char* buffer, size_t size) {
  char directory[kMaxPathLength] = {};
  if (!BuildDirectoryPath(state, directory, sizeof(directory))) {
    return false;
  }
  mkdir(directory, 0777);

  hal::RtcStatus status;
  if (state->rtc != nullptr && state->rtc->ReadRtcStatus(&status) &&
      status.ready && status.year >= 2000) {
    return std::snprintf(buffer, size, "%s/%04d%02d%02d-%02d%02d%02d.wav",
               directory, static_cast<int>(status.year),
               static_cast<int>(status.month), static_cast<int>(status.day),
               static_cast<int>(status.hour), static_cast<int>(status.minute),
               static_cast<int>(status.second)) < static_cast<int>(size);
  }
  // Without a valid clock fall back to a counter so names stay unique.
  for (unsigned index = 1; index < 1000; ++index) {
    if (std::snprintf(buffer, size, "%s/memo-%03u.wav", directory, index) >=
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
 * @brief Repaints the record button for the current state.
 * @param state View state.
 */
void ApplyRecordButtonState(VoiceMemoViewState* state) {
  if (state->record_inner == nullptr) {
    return;
  }
  const int size =
      state->recording ? kRecordInnerActiveSize : kRecordInnerIdleSize;
  lv_obj_set_size(state->record_inner, size, size);
  lv_obj_set_style_radius(state->record_inner,
      state->recording ? 14 : LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_center(state->record_inner);
}

/**
 * @brief Starts or stops recording when the big button is tapped.
 * @param event LVGL event.
 */
void RecordClickedCallback(lv_event_t* event) {
  auto* state =
      static_cast<VoiceMemoViewState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->audio == nullptr) {
    return;
  }

  if (state->recording) {
    PlayUiHapticEffect(HapticEffect::kImpactMedium);
    state->audio->StopAudioRecording();
    state->recording = false;
    ApplyRecordButtonState(state);
    if (state->status_label != nullptr) {
      lv_label_set_text(state->status_label, "Saving…");
    }
    return;
  }

  StopPlayback(state);
  char path[kMaxPathLength] = {};
  if (!BuildNewRecordingPath(state, path, sizeof(path))) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    if (state->status_label != nullptr) {
      lv_label_set_text(state->status_label, "No SD card");
    }
    return;
  }
  if (!state->audio->StartAudioRecording(path)) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    if (state->status_label != nullptr) {
      lv_label_set_text(state->status_label, "Could not start recording");
    }
    return;
  }

  PlayUiHapticEffect(HapticEffect::kImpactHeavy);
  std::snprintf(state->pending_path, sizeof(state->pending_path), "%s", path);
  state->recording = true;
  ApplyRecordButtonState(state);
}

/**
 * @brief Keeps the timer, level meter and list in step with the hardware.
 * @param timer LVGL timer.
 */
void RefreshTimerCallback(lv_timer_t* timer) {
  auto* state =
      static_cast<VoiceMemoViewState*>(lv_timer_get_user_data(timer));
  if (state == nullptr || state->audio == nullptr) {
    return;
  }

  hal::AudioRecordingStatus recording_status;
  const bool has_recording_status =
      state->audio->ReadAudioRecordingStatus(&recording_status);

  if (state->recording && has_recording_status) {
    char text[48] = {};
    FormatDuration(text, sizeof(text), recording_status.elapsed_ms);
    if (state->status_label != nullptr) {
      lv_label_set_text(state->status_label, text);
    }
    if (state->meter != nullptr) {
      const int width = std::max(4,
          static_cast<int>(lv_obj_get_width(state->root)) - 2 * kSidePadding);
      lv_obj_set_width(state->meter,
          std::max(4, width * recording_status.level_percent / 100));
    }
    if (recording_status.state == hal::AudioRecordingState::kError) {
      state->recording = false;
      ApplyRecordButtonState(state);
      PlayUiHapticEffect(HapticEffect::kWarning);
      if (state->status_label != nullptr) {
        lv_label_set_text(state->status_label, "Recording failed");
      }
    }
    return;
  }

  if (!state->recording && state->pending_path[0] != '\0' &&
      has_recording_status &&
      recording_status.state != hal::AudioRecordingState::kRecording) {
    // The capture task has closed the file, so the new memo can be listed.
    state->pending_path[0] = '\0';
    if (state->meter != nullptr) {
      lv_obj_set_width(state->meter, 4);
    }
    if (state->status_label != nullptr) {
      lv_label_set_text(state->status_label, "");
    }
    ScanRecordings(state);
    RebuildList(state);
    PlayUiHapticEffect(HapticEffect::kSuccess);
    return;
  }

  if (state->playing_index >= 0) {
    hal::AudioFilePlaybackStatus playback;
    if (state->audio->ReadAudioFileStatus(&playback) &&
        playback.state != hal::AudioFilePlaybackState::kPlaying &&
        playback.state != hal::AudioFilePlaybackState::kPaused) {
      state->playing_index = -1;
      RebuildList(state);
    }
  }
}

/**
 * @brief Tears down timers and hardware use when the view is deleted.
 * @param event LVGL event.
 */
void ViewDeletedCallback(lv_event_t* event) {
  auto* state =
      static_cast<VoiceMemoViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  if (state->timer != nullptr) {
    lv_timer_delete(state->timer);
    state->timer = nullptr;
  }
  if (state->recording && state->audio != nullptr) {
    state->audio->StopAudioRecording();
  }
  StopPlayback(state);
  delete state;
}

}  // namespace

lv_obj_t* CreateVoiceMemoView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config) {
  (void)app_entry;
  if (parent == nullptr || config.width <= 0 || config.height <= 0) {
    return nullptr;
  }

  lv_obj_t* container = lv_obj_create(parent);
  if (container == nullptr) {
    return nullptr;
  }
  lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(container, config.width, config.height);
  lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(container, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(container,
      lv_color_hex(theme::ActiveThemeColors().surface), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container, LV_OPA_COVER, LV_PART_MAIN);

  auto* state = new (std::nothrow) VoiceMemoViewState();
  if (state == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  state->root = container;
  state->audio = config.audio;
  state->storage = config.storage;
  state->rtc = config.rtc;
  lv_obj_add_event_cb(container, ViewDeletedCallback, LV_EVENT_DELETE, state);

  lv_obj_t* title = CreateLabel(container, "Voice Memos",
      theme::ActiveThemeColors().on_surface, &lvgl_font_google_sans_flex_36);
  if (title != nullptr) {
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, kSidePadding, kHeaderHeight - 56);
  }

  state->list = lv_obj_create(container);
  if (state->list == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  MakeTransparent(state->list);
  lv_obj_set_size(state->list, config.width - 2 * kSidePadding,
      config.height - kHeaderHeight - kTransportHeight);
  lv_obj_set_pos(state->list, kSidePadding, kHeaderHeight);
  lv_obj_set_scroll_dir(state->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_OFF);

  // Transport area along the bottom: level meter, timer and the record button.
  state->meter = lv_obj_create(container);
  if (state->meter != nullptr) {
    lv_obj_remove_flag(state->meter, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state->meter, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(state->meter, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->meter, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state->meter, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->meter, lv_color_hex(kMeterColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->meter, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_size(state->meter, 4, 6);
    lv_obj_align(state->meter, LV_ALIGN_BOTTOM_LEFT, kSidePadding,
        -kTransportHeight + 26);
  }

  state->status_label = CreateLabel(container, "",
      theme::ActiveThemeColors().on_surface_variant,
      &lvgl_font_google_sans_flex_28);
  if (state->status_label != nullptr) {
    lv_obj_align(state->status_label, LV_ALIGN_BOTTOM_MID, 0,
        -kTransportHeight + 60);
  }

  state->record_button = lv_button_create(container);
  if (state->record_button == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  lv_obj_set_size(state->record_button, kRecordButtonSize, kRecordButtonSize);
  lv_obj_align(state->record_button, LV_ALIGN_BOTTOM_MID, 0, -56);
  lv_obj_set_style_radius(state->record_button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(state->record_button, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(state->record_button, 5, LV_PART_MAIN);
  lv_obj_set_style_border_color(state->record_button,
      lv_color_hex(theme::ActiveThemeColors().outline), LV_PART_MAIN);
  lv_obj_set_style_pad_all(state->record_button, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(
      state->record_button, RecordClickedCallback, LV_EVENT_CLICKED, state);

  state->record_inner = lv_obj_create(state->record_button);
  if (state->record_inner != nullptr) {
    lv_obj_remove_flag(state->record_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state->record_inner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(state->record_inner, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->record_inner, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->record_inner, lv_color_hex(kRecordColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->record_inner, LV_OPA_COVER, LV_PART_MAIN);
    ApplyRecordButtonState(state);
  }

  if (state->storage != nullptr) {
    state->storage->EnsureSdCardMounted();
  }
  ScanRecordings(state);
  lv_obj_update_layout(state->list);
  RebuildList(state);

  state->timer = lv_timer_create(RefreshTimerCallback, kRefreshIntervalMs,
      state);
  if (state->timer == nullptr) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Create voice memo timer failed\n");
  }
  return container;
}

}  // namespace lilygo_box::ui
