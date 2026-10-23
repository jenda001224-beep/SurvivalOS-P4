/*
 * @Description: Photos app view
 * @License: GPL 3.0
 */
#include "ui/views/photos_view.h"

#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "app/edition.h"
#include "hal/providers/storage_provider.h"
#include "survival/avi_reader.h"
#include "survival/jpeg_image.h"
#include "ui/haptic_feedback.h"
#include "ui/input/back_navigation_controller.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"

namespace lilygo_box::ui {
namespace {

constexpr const char* kPhotoDirectory = "DCIM";
constexpr size_t kMaxPhotos = 240;
constexpr size_t kMaxPathLength = 160;

constexpr int kGridColumns = 3;
constexpr int kGridGap = 6;
constexpr int kHeaderHeight = 108;
constexpr int kSidePadding = 12;
// Thumbnails are decoded a few at a time so the grid stays responsive while it
// fills in, the way a phone gallery does.
constexpr uint32_t kThumbnailTimerMs = 20;
constexpr int kThumbnailsPerTick = 1;

// One photo or video on the card.
struct Photo {
  char name[64] = {};
  char path[kMaxPathLength] = {};
  bool video = false;
  survival::JpegImage thumbnail;
  lv_image_dsc_t thumbnail_descriptor = {};
  lv_obj_t* cell = nullptr;
  lv_obj_t* image = nullptr;
  bool thumbnail_ready = false;
  bool thumbnail_failed = false;
};

struct PhotosViewState {
  hal::StorageProvider* storage = nullptr;
  lv_obj_t* root = nullptr;
  lv_obj_t* grid = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* empty_label = nullptr;
  lv_timer_t* thumbnail_timer = nullptr;

  // Full-screen viewer, created on demand.
  lv_obj_t* viewer = nullptr;
  lv_obj_t* viewer_image = nullptr;
  lv_obj_t* viewer_caption = nullptr;
  survival::JpegImage viewer_photo;
  lv_image_dsc_t viewer_descriptor = {};
  int viewer_index = -1;

  // Video playback state. Frames are decoded one per timer tick, which is what
  // keeps the UI responsive while a video runs.
  survival::AviReader video;
  lv_timer_t* video_timer = nullptr;
  uint8_t* video_frame = nullptr;
  uint32_t video_frame_capacity = 0;
  uint32_t video_position = 0;
  bool video_playing = false;

  Photo* photos = nullptr;
  size_t photo_count = 0;
  size_t next_thumbnail = 0;
  int thumbnail_size = 0;
  int view_width = 0;
  int view_height = 0;
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
 * @brief Fills an LVGL image descriptor from a decoded image.
 * @param descriptor Descriptor to fill.
 * @param image Decoded RGB565 image.
 */
void FillImageDescriptor(
    lv_image_dsc_t* descriptor, const survival::JpegImage& image) {
  std::memset(descriptor, 0, sizeof(*descriptor));
  descriptor->header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor->header.cf = LV_COLOR_FORMAT_RGB565;
  descriptor->header.w = static_cast<uint32_t>(image.width);
  descriptor->header.h = static_cast<uint32_t>(image.height);
  descriptor->header.stride =
      static_cast<uint32_t>(image.width * sizeof(uint16_t));
  descriptor->data = image.pixels;
  descriptor->data_size =
      static_cast<uint32_t>(image.width * image.height * sizeof(uint16_t));
}

/**
 * @brief Builds the directory the photos live in.
 * @param state View state.
 * @param buffer Output buffer.
 * @param size Buffer size.
 * @return true when the SD card is mounted and the path was built.
 */
bool BuildDirectoryPath(
    const PhotosViewState* state, char* buffer, size_t size) {
  // SurvivalOS: without a card the camera saves to internal memory.
  const char* base = app::MediaBasePath(state->storage, false, nullptr);
  if (base == nullptr) {
    return false;
  }
  return std::snprintf(buffer, size, "%s/%s", base, kPhotoDirectory) <
      static_cast<int>(size);
}

/**
 * @brief Reads the photo list off the card, newest first.
 * @param state View state.
 */
void ScanPhotos(PhotosViewState* state) {
  state->photo_count = 0;
  state->next_thumbnail = 0;
  char directory[kMaxPathLength] = {};
  if (!BuildDirectoryPath(state, directory, sizeof(directory))) {
    return;
  }

  DIR* handle = opendir(directory);
  if (handle == nullptr) {
    return;
  }
  while (state->photo_count < kMaxPhotos) {
    const dirent* entry = readdir(handle);
    if (entry == nullptr) {
      break;
    }
    const size_t length = std::strlen(entry->d_name);
    const bool is_jpeg = (length > 4 &&
                             strcasecmp(entry->d_name + length - 4, ".jpg") ==
                                 0) ||
        (length > 5 &&
            strcasecmp(entry->d_name + length - 5, ".jpeg") == 0);
    const bool is_video = length > 4 &&
        strcasecmp(entry->d_name + length - 4, ".avi") == 0;
    if (!is_jpeg && !is_video) {
      continue;
    }
    Photo& photo = state->photos[state->photo_count];
    photo.video = is_video;
    std::snprintf(photo.name, sizeof(photo.name), "%s", entry->d_name);
    std::snprintf(
        photo.path, sizeof(photo.path), "%s/%s", directory, entry->d_name);
    ++state->photo_count;
  }
  closedir(handle);

  // Capture names sort chronologically, so reverse order is newest first.
  std::sort(state->photos, state->photos + state->photo_count,
      [](const Photo& left, const Photo& right) {
        return std::strcmp(left.name, right.name) > 0;
      });
}

/**
 * @brief Releases every decoded thumbnail.
 * @param state View state.
 */
void ReleaseThumbnails(PhotosViewState* state) {
  for (size_t index = 0; index < state->photo_count; ++index) {
    survival::FreeJpegImage(&state->photos[index].thumbnail);
    state->photos[index].thumbnail_ready = false;
  }
}

void CloseViewer(PhotosViewState* state);
void ViewerGestureCallback(lv_event_t* event);
void StopVideo(PhotosViewState* state);

/**
 * @brief Shows one decoded video frame in the viewer.
 * @param timer LVGL timer.
 */
void VideoTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<PhotosViewState*>(lv_timer_get_user_data(timer));
  if (state == nullptr || !state->video_playing) {
    return;
  }
  if (state->video_position >= state->video.frame_count()) {
    // Loop, which is what a short clip wants; there is no transport bar to
    // restart it with.
    state->video_position = 0;
  }

  uint32_t size = 0;
  if (!state->video.ReadFrame(state->video_position, state->video_frame,
          state->video_frame_capacity, &size)) {
    StopVideo(state);
    return;
  }
  ++state->video_position;

  survival::JpegImage frame;
  if (!survival::DecodeJpegBuffer(state->video_frame, size,
          static_cast<uint32_t>(state->view_width),
          static_cast<uint32_t>(state->view_height), &frame)) {
    return;
  }
  survival::FreeJpegImage(&state->viewer_photo);
  state->viewer_photo = frame;
  FillImageDescriptor(&state->viewer_descriptor, state->viewer_photo);
  if (state->viewer_image != nullptr) {
    lv_image_set_src(state->viewer_image, &state->viewer_descriptor);
    lv_obj_center(state->viewer_image);
  }
  if (state->viewer_caption != nullptr) {
    char caption[96] = {};
    const uint32_t seconds = state->video_position /
        (state->video.frames_per_second() == 0
                ? 15
                : state->video.frames_per_second());
    const unsigned int total_seconds =
        static_cast<unsigned int>(state->video.duration_ms() / 1000U);
    std::snprintf(caption, sizeof(caption), "%s   %u:%02u / %u:%02u",
        state->photos[state->viewer_index].name,
        static_cast<unsigned int>(seconds) / 60U,
        static_cast<unsigned int>(seconds) % 60U, total_seconds / 60U,
        total_seconds % 60U);
    lv_label_set_text(state->viewer_caption, caption);
  }
}

/**
 * @brief Stops playback and releases the video's buffers.
 * @param state View state.
 */
void StopVideo(PhotosViewState* state) {
  if (state->video_timer != nullptr) {
    lv_timer_delete(state->video_timer);
    state->video_timer = nullptr;
  }
  state->video_playing = false;
  state->video_position = 0;
  state->video.Close();
  heap_caps_free(state->video_frame);
  state->video_frame = nullptr;
  state->video_frame_capacity = 0;
}

/**
 * @brief Starts playing a video in the viewer.
 * @param state View state.
 * @param index Index into the photo list.
 * @return true when playback started.
 */
bool StartVideo(PhotosViewState* state, int index) {
  StopVideo(state);
  if (!state->video.Open(state->photos[index].path) ||
      state->video.frame_count() == 0) {
    return false;
  }
  state->video_frame_capacity = state->video.largest_frame();
  state->video_frame = static_cast<uint8_t*>(heap_caps_malloc(
      state->video_frame_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (state->video_frame == nullptr) {
    state->video.Close();
    return false;
  }

  const uint32_t fps = state->video.frames_per_second();
  state->video_position = 0;
  state->video_playing = true;
  state->video_timer = lv_timer_create(
      VideoTimerCallback, 1000U / (fps == 0 ? 15 : fps), state);
  if (state->video_timer == nullptr) {
    StopVideo(state);
    return false;
  }
  // Draw the first frame straight away rather than after one tick.
  VideoTimerCallback(state->video_timer);
  return true;
}

/**
 * @brief Shows one photo full screen.
 * @param state View state.
 * @param index Index into the photo list.
 */
void OpenViewer(PhotosViewState* state, int index) {
  if (index < 0 || index >= static_cast<int>(state->photo_count)) {
    return;
  }
  StopVideo(state);
  const bool video = state->photos[index].video;
  if (!video) {
    survival::FreeJpegImage(&state->viewer_photo);
    if (!survival::LoadJpegImage(state->photos[index].path,
            static_cast<uint32_t>(state->view_width),
            static_cast<uint32_t>(state->view_height),
            &state->viewer_photo)) {
      PlayUiHapticEffect(HapticEffect::kWarning);
      return;
    }
  }
  state->viewer_index = index;
  if (!video) {
    FillImageDescriptor(&state->viewer_descriptor, state->viewer_photo);
  }

  if (state->viewer == nullptr) {
    state->viewer = lv_obj_create(state->root);
    if (state->viewer == nullptr) {
      return;
    }
    lv_obj_remove_flag(state->viewer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(state->viewer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(state->viewer, state->view_width, state->view_height);
    lv_obj_set_pos(state->viewer, 0, 0);
    lv_obj_set_style_radius(state->viewer, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->viewer, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->viewer, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->viewer, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->viewer, LV_OPA_COVER, LV_PART_MAIN);

    state->viewer_image = lv_image_create(state->viewer);
    if (state->viewer_image != nullptr) {
      lv_obj_center(state->viewer_image);
    }
    state->viewer_caption =
        CreateLabel(state->viewer, "", 0xFFFFFF,
            &lvgl_font_google_sans_flex_22);
    if (state->viewer_caption != nullptr) {
      lv_obj_align(state->viewer_caption, LV_ALIGN_TOP_MID, 0, 70);
    }

    // Swiping sideways steps through the library without leaving the viewer.
    lv_obj_add_event_cb(
        state->viewer, ViewerGestureCallback, LV_EVENT_GESTURE, state);

    // Inside the viewer the back gesture closes the photo instead of the app.
    RegisterConditionalBackNavigationHandler(state->viewer, [state]() {
      CloseViewer(state);
      return true;
    });
  }

  lv_obj_remove_flag(state->viewer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_to_index(state->viewer, -1);
  if (video) {
    if (!StartVideo(state, index)) {
      PlayUiHapticEffect(HapticEffect::kWarning);
      CloseViewer(state);
    }
    return;
  }

  if (state->viewer_image != nullptr) {
    lv_image_set_src(state->viewer_image, &state->viewer_descriptor);
    lv_obj_center(state->viewer_image);
  }
  if (state->viewer_caption != nullptr) {
    char caption[96] = {};
    std::snprintf(caption, sizeof(caption), "%s   %ux%u  (%d/%u)",
        state->photos[index].name,
        static_cast<unsigned int>(state->viewer_photo.source_width),
        static_cast<unsigned int>(state->viewer_photo.source_height),
        index + 1, static_cast<unsigned int>(state->photo_count));
    lv_label_set_text(state->viewer_caption, caption);
  }
  lv_obj_remove_flag(state->viewer, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_to_index(state->viewer, -1);
}

/**
 * @brief Hides the full-screen viewer and frees its decoded photo.
 * @param state View state.
 */
void CloseViewer(PhotosViewState* state) {
  StopVideo(state);
  if (state->viewer != nullptr) {
    lv_obj_add_flag(state->viewer, LV_OBJ_FLAG_HIDDEN);
  }
  if (state->viewer_image != nullptr) {
    lv_image_set_src(state->viewer_image, nullptr);
  }
  survival::FreeJpegImage(&state->viewer_photo);
  state->viewer_index = -1;
}

/**
 * @brief Opens the tapped photo full screen.
 * @param event LVGL event.
 */
void CellClickedCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  lv_obj_t* cell = lv_event_get_target_obj(event);
  if (state == nullptr || cell == nullptr) {
    return;
  }
  const int index =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell)));
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  OpenViewer(state, index);
}

/**
 * @brief Steps to the next or previous photo, or closes on a tap.
 * @param event LVGL event.
 */
void ViewerGestureCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->viewer_index < 0) {
    return;
  }
  const lv_dir_t direction = lv_indev_get_gesture_dir(lv_indev_active());
  int next = state->viewer_index;
  if (direction == LV_DIR_LEFT) {
    next = state->viewer_index + 1;
  } else if (direction == LV_DIR_RIGHT) {
    next = state->viewer_index - 1;
  } else {
    return;
  }
  if (next < 0 || next >= static_cast<int>(state->photo_count)) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  OpenViewer(state, next);
}

/**
 * @brief Lays out the grid cells; thumbnails arrive later.
 * @param state View state.
 */
void RebuildGrid(PhotosViewState* state) {
  if (state->grid == nullptr) {
    return;
  }
  lv_obj_clean(state->grid);

  if (state->photo_count == 0) {
    const bool card_mounted =
        state->storage != nullptr && state->storage->IsSdCardMounted();
    lv_obj_t* empty = CreateLabel(state->grid,
        card_mounted || app::GetEdition() == app::Edition::kMini
            ? "No photos yet.\nTake one with the Camera app."
            : "No photos yet.\nWithout an SD card they are kept\nin the "
              "phone's own memory.",
        0x8A8A8E, &lvgl_font_google_sans_flex_24);
    if (empty != nullptr) {
      lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_align(empty, LV_ALIGN_TOP_MID, 0, 60);
    }
    return;
  }

  for (size_t index = 0; index < state->photo_count; ++index) {
    Photo& photo = state->photos[index];
    lv_obj_t* cell = lv_obj_create(state->grid);
    if (cell == nullptr) {
      return;
    }
    lv_obj_remove_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(cell, state->thumbnail_size, state->thumbnail_size);
    const int column = static_cast<int>(index) % kGridColumns;
    const int row = static_cast<int>(index) / kGridColumns;
    lv_obj_set_pos(cell, column * (state->thumbnail_size + kGridGap),
        row * (state->thumbnail_size + kGridGap));
    lv_obj_set_style_radius(cell, 10, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(cell, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cell,
        lv_color_hex(theme::ActiveThemeColors().surface_container_high),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_user_data(
        cell, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(cell, CellClickedCallback, LV_EVENT_CLICKED, state);
    photo.cell = cell;
    photo.image = nullptr;

    if (photo.thumbnail_ready) {
      lv_obj_t* image = lv_image_create(cell);
      if (image != nullptr) {
        lv_image_set_src(image, &photo.thumbnail_descriptor);
        lv_obj_center(image);
        photo.image = image;
      }
    }
    if (photo.video) {
      lv_obj_t* badge = CreateLabel(cell, icon::kPlayCircle, 0xFFFFFF,
          &lvgl_font_material_symbols_fill_32);
      if (badge != nullptr) {
        lv_obj_align(badge, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
      }
    }
  }
}

/**
 * @brief Decodes the next few thumbnails and drops them into their cells.
 * @param timer LVGL timer.
 */
void ThumbnailTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<PhotosViewState*>(lv_timer_get_user_data(timer));
  if (state == nullptr) {
    return;
  }
  if (state->next_thumbnail >= state->photo_count) {
    lv_timer_delete(timer);
    state->thumbnail_timer = nullptr;
    return;
  }

  for (int produced = 0;
      produced < kThumbnailsPerTick && state->next_thumbnail <
          state->photo_count;
      ++produced) {
    Photo& photo = state->photos[state->next_thumbnail];
    ++state->next_thumbnail;
    if (photo.thumbnail_ready || photo.thumbnail_failed) {
      continue;
    }
    const uint32_t box = static_cast<uint32_t>(state->thumbnail_size);
    bool decoded = false;
    if (photo.video) {
      // A video's first frame is its thumbnail.
      survival::AviReader reader;
      if (reader.Open(photo.path) && reader.frame_count() > 0) {
        const uint32_t capacity = reader.largest_frame();
        auto* frame = static_cast<uint8_t*>(heap_caps_malloc(
            capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        uint32_t size = 0;
        if (frame != nullptr && reader.ReadFrame(0, frame, capacity, &size)) {
          decoded = survival::DecodeJpegBuffer(
              frame, size, box, box, &photo.thumbnail);
        }
        heap_caps_free(frame);
      }
    } else {
      decoded = survival::LoadJpegImage(photo.path, box, box,
          &photo.thumbnail);
    }
    if (!decoded) {
      photo.thumbnail_failed = true;
      continue;
    }
    photo.thumbnail_ready = true;
    FillImageDescriptor(&photo.thumbnail_descriptor, photo.thumbnail);
    if (photo.cell != nullptr && photo.image == nullptr) {
      lv_obj_t* image = lv_image_create(photo.cell);
      if (image != nullptr) {
        lv_image_set_src(image, &photo.thumbnail_descriptor);
        lv_obj_center(image);
        photo.image = image;
      }
    }
  }
}

/**
 * @brief Frees everything the view owns.
 * @param event LVGL event.
 */
void ViewDeletedCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  if (state->thumbnail_timer != nullptr) {
    lv_timer_delete(state->thumbnail_timer);
    state->thumbnail_timer = nullptr;
  }
  StopVideo(state);
  ReleaseThumbnails(state);
  survival::FreeJpegImage(&state->viewer_photo);
  delete[] state->photos;
  delete state;
}

}  // namespace

lv_obj_t* CreatePhotosView(lv_obj_t* parent, const app::AppEntry& app_entry,
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

  auto* state = new (std::nothrow) PhotosViewState();
  if (state == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  state->photos = new (std::nothrow) Photo[kMaxPhotos];
  if (state->photos == nullptr) {
    delete state;
    lv_obj_delete(container);
    return nullptr;
  }
  state->root = container;
  state->storage = config.storage;
  state->view_width = config.width;
  state->view_height = config.height;
  state->thumbnail_size =
      (config.width - 2 * kSidePadding - (kGridColumns - 1) * kGridGap) /
      kGridColumns;
  lv_obj_add_event_cb(container, ViewDeletedCallback, LV_EVENT_DELETE, state);

  state->title = CreateLabel(container, "Photos",
      theme::ActiveThemeColors().on_surface, &lvgl_font_google_sans_flex_36);
  if (state->title != nullptr) {
    lv_obj_align(state->title, LV_ALIGN_TOP_LEFT, kSidePadding + 8,
        kHeaderHeight - 56);
  }

  state->grid = lv_obj_create(container);
  if (state->grid == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  MakeTransparent(state->grid);
  lv_obj_set_size(state->grid, config.width - 2 * kSidePadding,
      config.height - kHeaderHeight);
  lv_obj_set_pos(state->grid, kSidePadding, kHeaderHeight);
  lv_obj_set_scroll_dir(state->grid, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->grid, LV_SCROLLBAR_MODE_OFF);

  if (state->storage != nullptr &&
      app::GetEdition() == app::Edition::kFull) {
    state->storage->EnsureSdCardMounted();
  }
  ScanPhotos(state);
  RebuildGrid(state);

  if (state->photo_count > 0) {
    state->thumbnail_timer =
        lv_timer_create(ThumbnailTimerCallback, kThumbnailTimerMs, state);
    if (state->thumbnail_timer == nullptr) {
      LogMessage(LogLevel::kError, __FILE__, __LINE__,
          "Create thumbnail timer failed\n");
    }
  }
  return container;
}

}  // namespace lilygo_box::ui
