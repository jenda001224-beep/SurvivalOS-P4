/*
 * @Description: Photos app view
 * @License: GPL 3.0
 */
#include "ui/views/photos_view.h"

#include <dirent.h>
#include <strings.h>

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "app/edition.h"
#include "app/photo_trash.h"
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
// Thumbnails are made by a worker task below the UI's priority, visible cells
// first, and kept on the card in DCIM/.thumbs so a second visit only reads
// 60 kB per cell instead of decoding a 1080p JPEG. The UI timer only hangs the
// finished ones into their cells.
constexpr uint32_t kThumbnailTimerMs = 40;
constexpr const char* kThumbnailCacheDirectory = ".thumbs";
constexpr uint32_t kThumbnailCacheMagic = 0x48545653;  // "SVTH"
// Rows kept decoded above and below the visible ones; the rest are dropped so
// a card with hundreds of photos does not fill the PSRAM.
constexpr int kKeepRowsBehind = 4;
constexpr int kKeepRowsAhead = 6;
constexpr uint32_t kWorkerStackBytes = 6144;
constexpr UBaseType_t kWorkerPriority = 2;
constexpr TickType_t kWorkerIdleTicks = pdMS_TO_TICKS(30);
// A swipe has to travel this far, mostly sideways, to change the photo.
constexpr int kSwipeDistance = 70;
// Presses that start this close to the left edge belong to the back gesture.
constexpr int kBackEdgeWidth = 36;
// A double tap zooms to this many times the size that fits the screen.
constexpr int kZoomFactor = 3;
constexpr int kActionBarHeight = 128;
constexpr int kTrashNoteHeight = 74;

enum ThumbnailState : uint8_t {
  kThumbnailNone = 0,
  kThumbnailWorking = 1,
  kThumbnailReady = 2,
  kThumbnailFailed = 3,
};

// One photo or video on the card.
struct Photo {
  char name[64] = {};
  char path[kMaxPathLength] = {};
  bool video = false;
  survival::JpegImage thumbnail;
  lv_image_dsc_t thumbnail_descriptor = {};
  lv_obj_t* cell = nullptr;
  lv_obj_t* image = nullptr;
  lv_obj_t* check = nullptr;  // selection mark, in Select mode
  bool selected = false;
  // Written by the worker (None -> Working -> Ready/Failed) and by the UI
  // (Ready -> None when the thumbnail is dropped); never both on one state.
  std::atomic<uint8_t> thumbnail_state{kThumbnailNone};

  Photo() = default;
  // Moves are for sorting the list, which happens before the worker starts.
  Photo(Photo&& other) noexcept { *this = std::move(other); }
  Photo& operator=(Photo&& other) noexcept {
    std::memcpy(name, other.name, sizeof(name));
    std::memcpy(path, other.path, sizeof(path));
    video = other.video;
    thumbnail = other.thumbnail;
    thumbnail_descriptor = other.thumbnail_descriptor;
    cell = other.cell;
    image = other.image;
    check = other.check;
    selected = other.selected;
    thumbnail_state.store(other.thumbnail_state.load());
    return *this;
  }
};

struct PhotosViewState {
  hal::StorageProvider* storage = nullptr;
  lv_obj_t* root = nullptr;
  lv_obj_t* grid = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* empty_label = nullptr;
  lv_timer_t* thumbnail_timer = nullptr;
  // Thumbnail worker.
  TaskHandle_t worker = nullptr;
  SemaphoreHandle_t worker_done = nullptr;
  std::atomic<bool> worker_stop{false};
  // Range the worker may fill, in photo indices, visible ones first.
  std::atomic<int> visible_first{0};
  std::atomic<int> visible_last{0};
  std::atomic<int> keep_first{0};
  std::atomic<int> keep_last{0};
  bool cache_on_card = false;
  char cache_directory[kMaxPathLength] = {};

  // Full-screen viewer, created on demand.
  lv_obj_t* viewer = nullptr;
  lv_obj_t* viewer_image = nullptr;
  lv_obj_t* viewer_caption = nullptr;
  lv_obj_t* viewer_back = nullptr;
  // SurvivalOS: zoom and delete. The photo sits in a pan box that scrolls
  // when zoomed in; a double tap zooms in at that point and back out.
  lv_obj_t* viewer_pan = nullptr;
  lv_obj_t* viewer_delete = nullptr;
  lv_obj_t* viewer_restore = nullptr;
  lv_obj_t* delete_dialog = nullptr;
  // SurvivalOS: Select and Recently Deleted.
  bool trash_mode = false;
  bool select_mode = false;
  int trash_count = 0;
  bool dialog_for_selection = false;
  lv_obj_t* header_back = nullptr;
  lv_obj_t* trash_button = nullptr;
  lv_obj_t* trash_button_label = nullptr;
  lv_obj_t* select_button = nullptr;
  lv_obj_t* select_button_label = nullptr;
  lv_obj_t* trash_note = nullptr;
  lv_obj_t* action_bar = nullptr;
  lv_obj_t* action_label = nullptr;
  lv_obj_t* action_all = nullptr;
  lv_obj_t* action_restore = nullptr;
  lv_obj_t* action_delete = nullptr;
  bool zoomed = false;
  bool viewer_full_resolution = false;
  lv_point_t press_point = {};
  bool press_tracking = false;
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
  // Recently Deleted is a folder inside the photo folder.
  return (state->trash_mode
             ? std::snprintf(buffer, size, "%s/%s/%s", base, kPhotoDirectory,
                   app::kPhotoTrashDirectory)
             : std::snprintf(buffer, size, "%s/%s", base, kPhotoDirectory)) <
      static_cast<int>(size);
}

// The photo folder itself, whichever list is on screen.
bool BuildPhotoDirectoryPath(
    const PhotosViewState* state, char* buffer, size_t size) {
  const char* base = app::MediaBasePath(state->storage, false, nullptr);
  return base != nullptr &&
      std::snprintf(buffer, size, "%s/%s", base, kPhotoDirectory) < static_cast<int>(size);
}

// What the photo is called on screen: without the deletion time in the trash.
const char* DisplayName(const PhotosViewState* state, const Photo& photo) {
  return state->trash_mode ? app::OriginalPhotoName(photo.name) : photo.name;
}

const char* FileName(const Photo& photo) {
  const char* slash = std::strrchr(photo.path, '/');
  return slash != nullptr ? slash + 1 : photo.path;
}

/**
 * @brief Reads the photo list off the card, newest first.
 * @param state View state.
 */
void ScanPhotos(PhotosViewState* state) {
  state->photo_count = 0;
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

  // Capture names hold the time (IMG_20261031-080925.jpg, VID_...), so past
  // the prefix they sort chronologically and in reverse newest first. Without
  // skipping it every video would come before every photo.
  const auto time_part = [](const char* name) {
    while (*name != '\0' && (*name < '0' || *name > '9')) {
      ++name;
    }
    return name;
  };
  std::sort(state->photos, state->photos + state->photo_count,
      [&time_part](const Photo& left, const Photo& right) {
        return std::strcmp(time_part(left.name), time_part(right.name)) > 0;
      });
}

/**
 * @brief Releases every decoded thumbnail.
 * @param state View state.
 */
void ReleaseThumbnails(PhotosViewState* state) {
  for (size_t index = 0; index < state->photo_count; ++index) {
    lv_image_cache_drop(&state->photos[index].thumbnail_descriptor);
    survival::FreeJpegImage(&state->photos[index].thumbnail);
    state->photos[index].thumbnail_state.store(kThumbnailNone);
  }
}

// ── Thumbnail worker ─────────────────────────────────────────────────────────

/**
 * @brief Cache file of one photo's thumbnail.
 */
bool BuildThumbnailCachePath(const PhotosViewState* state, const Photo& photo,
    char* buffer, size_t size) {
  return state->cache_on_card &&
      std::snprintf(buffer, size, "%s/%s.thm", state->cache_directory,
          photo.name) < static_cast<int>(size);
}

struct ThumbnailCacheHeader {
  uint32_t magic;
  uint16_t width;
  uint16_t height;
  uint32_t source_bytes;
  uint32_t reserved;
};

uint32_t SourceBytes(const Photo& photo) {
  struct stat info = {};
  return stat(photo.path, &info) == 0 ? static_cast<uint32_t>(info.st_size) : 0;
}

bool ReadCachedThumbnail(const PhotosViewState* state, const Photo& photo,
    uint32_t source_bytes, survival::JpegImage* image) {
  char path[kMaxPathLength + 16] = {};
  if (!BuildThumbnailCachePath(state, photo, path, sizeof(path))) {
    return false;
  }
  FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  ThumbnailCacheHeader header = {};
  bool ok = std::fread(&header, sizeof(header), 1, file) == 1 &&
      header.magic == kThumbnailCacheMagic && header.width > 0 &&
      header.height > 0 &&
      header.width <= static_cast<uint32_t>(state->thumbnail_size) &&
      header.height <= static_cast<uint32_t>(state->thumbnail_size) &&
      header.source_bytes == source_bytes;
  const size_t bytes = static_cast<size_t>(header.width) * header.height * 2;
  uint8_t* pixels = nullptr;
  if (ok) {
    pixels = static_cast<uint8_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ok = pixels != nullptr && std::fread(pixels, 1, bytes, file) == bytes;
  }
  std::fclose(file);
  if (!ok) {
    heap_caps_free(pixels);
    return false;
  }
  *image = survival::JpegImage();
  image->pixels = pixels;
  image->size_bytes = bytes;
  image->width = header.width;
  image->height = header.height;
  return true;
}

void WriteCachedThumbnail(const PhotosViewState* state, const Photo& photo,
    uint32_t source_bytes, const survival::JpegImage& image) {
  char path[kMaxPathLength + 16] = {};
  if (source_bytes == 0 ||
      !BuildThumbnailCachePath(state, photo, path, sizeof(path))) {
    return;
  }
  FILE* file = std::fopen(path, "wb");
  if (file == nullptr) {
    return;
  }
  const ThumbnailCacheHeader header = {kThumbnailCacheMagic,
      static_cast<uint16_t>(image.width), static_cast<uint16_t>(image.height),
      source_bytes, 0};
  const size_t bytes = static_cast<size_t>(image.width) * image.height * 2;
  const bool ok = std::fwrite(&header, sizeof(header), 1, file) == 1 &&
      std::fwrite(image.pixels, 1, bytes, file) == bytes;
  std::fclose(file);
  if (!ok) {
    std::remove(path);
  }
}

bool MakeThumbnail(const PhotosViewState* state, const Photo& photo,
    survival::JpegImage* image) {
  const uint32_t box = static_cast<uint32_t>(state->thumbnail_size);
  const uint32_t source_bytes = state->cache_on_card ? SourceBytes(photo) : 0;
  if (source_bytes != 0 &&
      ReadCachedThumbnail(state, photo, source_bytes, image)) {
    return true;
  }
  bool made = false;
  if (photo.video) {
    // A video's first frame is its thumbnail.
    survival::AviReader reader;
    if (reader.Open(photo.path) && reader.frame_count() > 0) {
      const uint32_t capacity = reader.largest_frame();
      auto* frame = static_cast<uint8_t*>(heap_caps_malloc(
          capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      uint32_t size = 0;
      if (frame != nullptr && reader.ReadFrame(0, frame, capacity, &size)) {
        made = survival::DecodeJpegThumbnail(frame, size, box, image);
      }
      heap_caps_free(frame);
    }
  } else {
    made = survival::LoadJpegThumbnail(photo.path, box, image);
  }
  if (made) {
    WriteCachedThumbnail(state, photo, source_bytes, *image);
  }
  return made;
}

/**
 * @brief Next photo to make a thumbnail for: visible ones top to bottom, then
 *        the rows below, then those above.
 * @return Index, or -1 when everything in range is done.
 */
int PickThumbnail(PhotosViewState* state) {
  const int count = static_cast<int>(state->photo_count);
  const int first = std::max(0, state->visible_first.load());
  const int last = std::min(count - 1, state->visible_last.load());
  const int keep_first = std::max(0, state->keep_first.load());
  const int keep_last = std::min(count - 1, state->keep_last.load());
  auto free_slot = [&](int index) {
    return state->photos[index].thumbnail_state.load() == kThumbnailNone;
  };
  for (int index = first; index <= last; ++index) {
    if (free_slot(index)) return index;
  }
  for (int index = last + 1; index <= keep_last; ++index) {
    if (free_slot(index)) return index;
  }
  for (int index = first - 1; index >= keep_first; --index) {
    if (free_slot(index)) return index;
  }
  return -1;
}

void ThumbnailWorker(void* argument) {
  auto* state = static_cast<PhotosViewState*>(argument);
  while (!state->worker_stop.load()) {
    const int index = PickThumbnail(state);
    if (index < 0) {
      vTaskDelay(kWorkerIdleTicks);
      continue;
    }
    Photo& photo = state->photos[index];
    uint8_t expected = kThumbnailNone;
    if (!photo.thumbnail_state.compare_exchange_strong(
            expected, kThumbnailWorking)) {
      continue;
    }
    survival::JpegImage image;
    if (MakeThumbnail(state, photo, &image)) {
      photo.thumbnail = image;
      photo.thumbnail_state.store(kThumbnailReady);
    } else {
      photo.thumbnail_state.store(kThumbnailFailed);
    }
  }
  xSemaphoreGive(state->worker_done);
  vTaskDelete(nullptr);
}

/**
 * @brief Starts the worker for the photos found by ScanPhotos().
 */
void StartThumbnailWorker(PhotosViewState* state) {
  if (state->photo_count == 0 || state->worker != nullptr) {
    return;
  }
  char directory[kMaxPathLength] = {};
  bool on_card = false;
  const char* base = app::MediaBasePath(state->storage, false, &on_card);
  // Internal memory is too small to spend on a cache.
  state->cache_on_card = on_card && base != nullptr &&
      (state->trash_mode
              ? std::snprintf(directory, sizeof(directory), "%s/%s/%s/%s", base, kPhotoDirectory,
                    app::kPhotoTrashDirectory, kThumbnailCacheDirectory)
              : std::snprintf(directory, sizeof(directory), "%s/%s/%s", base, kPhotoDirectory,
                    kThumbnailCacheDirectory)) < static_cast<int>(sizeof(directory));
  if (state->cache_on_card) {
    mkdir(directory, 0775);
    std::snprintf(state->cache_directory, sizeof(state->cache_directory), "%s",
        directory);
  }
  state->worker_done = xSemaphoreCreateBinary();
  if (state->worker_done == nullptr) {
    return;
  }
  state->worker_stop.store(false);
  if (xTaskCreate(ThumbnailWorker, "photo_thumbs", kWorkerStackBytes, state,
          kWorkerPriority, &state->worker) != pdPASS) {
    state->worker = nullptr;
    vSemaphoreDelete(state->worker_done);
    state->worker_done = nullptr;
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Create thumbnail worker failed\n");
  }
}

/**
 * @brief Stops the worker and waits until it no longer touches the state.
 */
bool StopThumbnailWorker(PhotosViewState* state) {
  if (state->worker == nullptr) {
    return true;
  }
  state->worker_stop.store(true);
  // One thumbnail takes well under a second; a card that hangs must not hang
  // the UI forever, so after the wait the state is left to the worker.
  if (xSemaphoreTake(state->worker_done, pdMS_TO_TICKS(10000)) != pdTRUE) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Thumbnail worker did not stop\n");
    return false;
  }
  vSemaphoreDelete(state->worker_done);
  state->worker_done = nullptr;
  state->worker = nullptr;
  return true;
}

void CloseViewer(PhotosViewState* state);
void ViewerPressCallback(lv_event_t* event);
void ViewerDoubleClickedCallback(lv_event_t* event);
void ViewerDeleteClickedCallback(lv_event_t* event);
void PlaceViewerImage(PhotosViewState* state, bool zoomed, const lv_point_t* at);
void RebuildGrid(PhotosViewState* state);
void UpdateThumbnails(PhotosViewState* state);
void ViewerRestoreClickedCallback(lv_event_t* event);
void CloseDeleteDialog(PhotosViewState* state);
void ToggleSelection(PhotosViewState* state, int index);
void ViewerBackClickedCallback(lv_event_t* event);
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
    lv_image_cache_drop(&state->viewer_descriptor);
    lv_image_set_src(state->viewer_image, &state->viewer_descriptor);
    PlaceViewerImage(state, false, nullptr);
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
        DisplayName(state, state->photos[state->viewer_index]),
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
  state->zoomed = false;
  state->viewer_full_resolution = false;
  if (!video) {
    if (state->viewer_image != nullptr) {
      lv_image_set_src(state->viewer_image, nullptr);
    }
    lv_image_cache_drop(&state->viewer_descriptor);
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

    state->viewer_pan = lv_obj_create(state->viewer);
    if (state->viewer_pan == nullptr) {
      return;
    }
    lv_obj_set_size(state->viewer_pan, state->view_width, state->view_height);
    lv_obj_set_pos(state->viewer_pan, 0, 0);
    lv_obj_set_style_bg_opa(state->viewer_pan, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->viewer_pan, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->viewer_pan, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(state->viewer_pan, 0, LV_PART_MAIN);
    lv_obj_remove_flag(state->viewer_pan, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state->viewer_pan, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(state->viewer_pan, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(state->viewer_pan, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(state->viewer_pan, LV_DIR_ALL);
    lv_obj_set_scrollbar_mode(state->viewer_pan, LV_SCROLLBAR_MODE_OFF);
    state->viewer_image = lv_image_create(state->viewer_pan);
    if (state->viewer_image != nullptr) {
      lv_obj_remove_flag(state->viewer_image, LV_OBJ_FLAG_CLICKABLE);
      lv_image_set_inner_align(state->viewer_image, LV_IMAGE_ALIGN_STRETCH);
    }
    state->viewer_caption =
        CreateLabel(state->viewer, "", 0xFFFFFF,
            &lvgl_font_google_sans_flex_22);
    if (state->viewer_caption != nullptr) {
      lv_obj_align(state->viewer_caption, LV_ALIGN_TOP_MID, 40, 80);
    }

    // Swiping sideways steps through the library without leaving the viewer.
    // The swipe is measured here rather than through LVGL's gesture event,
    // which bubbles past the viewer to the screen and never arrived.
    lv_obj_remove_flag(state->viewer, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_remove_flag(state->viewer, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(
        state->viewer_pan, ViewerPressCallback, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(
        state->viewer_pan, ViewerPressCallback, LV_EVENT_RELEASED, state);
    lv_obj_add_event_cb(
        state->viewer_pan, ViewerPressCallback, LV_EVENT_PRESS_LOST, state);
    lv_obj_add_event_cb(state->viewer_pan, ViewerDoubleClickedCallback,
        LV_EVENT_DOUBLE_CLICKED, state);

    // A visible way back to the grid, next to the back gesture.
    state->viewer_back = lv_button_create(state->viewer);
    if (state->viewer_back != nullptr) {
      lv_obj_set_size(state->viewer_back, 84, 84);
      lv_obj_align(state->viewer_back, LV_ALIGN_TOP_LEFT, 16, 54);
      lv_obj_set_style_radius(state->viewer_back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(
          state->viewer_back, lv_color_hex(0x000000), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(state->viewer_back, LV_OPA_50, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(state->viewer_back, LV_OPA_80, LV_STATE_PRESSED);
      lv_obj_set_style_shadow_width(state->viewer_back, 0, LV_PART_MAIN);
      lv_obj_set_style_border_width(state->viewer_back, 0, LV_PART_MAIN);
      lv_obj_add_event_cb(state->viewer_back, ViewerBackClickedCallback,
          LV_EVENT_CLICKED, state);
      lv_obj_t* arrow = CreateLabel(state->viewer_back, icon::kArrowBack,
          0xFFFFFF, &lvgl_font_material_symbols_fill_32);
      if (arrow != nullptr) {
        lv_obj_center(arrow);
      }
    }

    state->viewer_delete = lv_button_create(state->viewer);
    if (state->viewer_delete != nullptr) {
      lv_obj_set_size(state->viewer_delete, 84, 84);
      lv_obj_align(state->viewer_delete, LV_ALIGN_BOTTOM_RIGHT, -24, -48);
      lv_obj_set_style_radius(state->viewer_delete, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(
          state->viewer_delete, lv_color_hex(0x000000), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(state->viewer_delete, LV_OPA_50, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(state->viewer_delete, LV_OPA_80, LV_STATE_PRESSED);
      lv_obj_set_style_shadow_width(state->viewer_delete, 0, LV_PART_MAIN);
      lv_obj_set_style_border_width(state->viewer_delete, 0, LV_PART_MAIN);
      lv_obj_add_event_cb(state->viewer_delete, ViewerDeleteClickedCallback,
          LV_EVENT_CLICKED, state);
      lv_obj_t* bin = CreateLabel(state->viewer_delete, icon::kDelete,
          0xFFFFFF, &lvgl_font_material_symbols_fill_32);
      if (bin != nullptr) {
        lv_obj_center(bin);
      }
    }

    state->viewer_restore = lv_button_create(state->viewer);
    if (state->viewer_restore != nullptr) {
      lv_obj_set_size(state->viewer_restore, 170, 84);
      lv_obj_align(state->viewer_restore, LV_ALIGN_BOTTOM_LEFT, 24, -48);
      lv_obj_set_style_radius(state->viewer_restore, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_bg_color(
          state->viewer_restore, lv_color_hex(0x000000), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(state->viewer_restore, LV_OPA_50, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(state->viewer_restore, LV_OPA_80, LV_STATE_PRESSED);
      lv_obj_set_style_shadow_width(state->viewer_restore, 0, LV_PART_MAIN);
      lv_obj_add_event_cb(state->viewer_restore, ViewerRestoreClickedCallback,
          LV_EVENT_CLICKED, state);
      lv_obj_t* text = CreateLabel(state->viewer_restore, "Restore", 0xFFFFFF,
          &lvgl_font_google_sans_flex_28);
      if (text != nullptr) {
        lv_obj_center(text);
      }
    }

    // Inside the viewer the back gesture closes the photo instead of the app.
    RegisterConditionalBackNavigationHandler(state->viewer, [state]() {
      if (state->delete_dialog != nullptr) {
        CloseDeleteDialog(state);
      } else {
        CloseViewer(state);
      }
      return true;
    });
  }
  if (state->viewer_restore != nullptr) {
    if (state->trash_mode) {
      lv_obj_remove_flag(state->viewer_restore, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(state->viewer_restore, LV_OBJ_FLAG_HIDDEN);
    }
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
    lv_image_cache_drop(&state->viewer_descriptor);
    lv_image_set_src(state->viewer_image, &state->viewer_descriptor);
    PlaceViewerImage(state, false, nullptr);
  }
  if (state->viewer_caption != nullptr) {
    char caption[96] = {};
    std::snprintf(caption, sizeof(caption), "%s   %ux%u  (%d/%u)",
        DisplayName(state, state->photos[index]),
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
  if (state->delete_dialog != nullptr) {
    lv_obj_delete(state->delete_dialog);
    state->delete_dialog = nullptr;
  }
  state->zoomed = false;
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
  if (state->select_mode) {
    ToggleSelection(state, index);
    return;
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  OpenViewer(state, index);
}

/**
 * @brief Moves the viewer one photo forward or back.
 */
void StepViewer(PhotosViewState* state, int step) {
  const int next = state->viewer_index + step;
  if (next < 0 || next >= static_cast<int>(state->photo_count)) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  OpenViewer(state, next);
}

void ViewerBackClickedCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  if (state != nullptr) {
    PlayUiHapticEffect(HapticEffect::kImpactLight);
    CloseViewer(state);
  }
}

/**
 * @brief Swipe left or right for the next or previous photo, down to go back
 *        to the grid; a tap near either side also steps.
 * @param event LVGL event.
 */
void ViewerPressCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  lv_indev_t* indev = lv_indev_active();
  if (state == nullptr || state->viewer_index < 0 || indev == nullptr) {
    return;
  }
  lv_point_t point = {};
  lv_indev_get_point(indev, &point);
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED) {
    state->press_point = point;
    state->press_tracking = point.x >= kBackEdgeWidth;
    return;
  }
  if (!state->press_tracking) {
    return;
  }
  state->press_tracking = false;
  if (code == LV_EVENT_PRESS_LOST) {
    return;
  }
  if (state->zoomed) {
    return;  // dragging moves the zoomed photo
  }
  const int dx = point.x - state->press_point.x;
  const int dy = point.y - state->press_point.y;
  const int adx = dx < 0 ? -dx : dx;
  const int ady = dy < 0 ? -dy : dy;
  if (adx >= kSwipeDistance && adx > ady) {
    StepViewer(state, dx < 0 ? 1 : -1);
  } else if (dy >= 2 * kSwipeDistance && ady > adx) {
    PlayUiHapticEffect(HapticEffect::kImpactLight);
    CloseViewer(state);
  } else if (adx < 20 && ady < 20) {
    // A tap on the outer fifths steps, like turning a page.
    if (point.x > state->view_width * 4 / 5) {
      StepViewer(state, 1);
    } else if (point.x < state->view_width / 5 && point.y > 160) {
      StepViewer(state, -1);
    }
  }
}

/**
 * @brief Sizes the photo to fit the screen, or kZoomFactor times that with the
 *        tapped point kept under the finger.
 */
void PlaceViewerImage(PhotosViewState* state, bool zoomed, const lv_point_t* at) {
  lv_obj_t* image = state->viewer_image;
  lv_obj_t* pan = state->viewer_pan;
  if (image == nullptr || pan == nullptr) {
    return;
  }
  const survival::JpegImage& photo = state->viewer_photo;
  const int width = std::max<int>(1, photo.width);
  const int height = std::max<int>(1, photo.height);
  // Fit inside the screen, never larger than the decoded photo.
  int fit_width = width;
  int fit_height = height;
  if (fit_width > state->view_width) {
    fit_height = fit_height * state->view_width / fit_width;
    fit_width = state->view_width;
  }
  if (fit_height > state->view_height) {
    fit_width = fit_width * state->view_height / fit_height;
    fit_height = state->view_height;
  }
  const int old_width = zoomed ? fit_width : lv_obj_get_width(image);
  const int old_height = zoomed ? fit_height : lv_obj_get_height(image);
  const int old_x = std::max(0, (state->view_width - old_width) / 2);
  const int old_y = std::max(0, (state->view_height - old_height) / 2);
  const int new_width = zoomed ? fit_width * kZoomFactor : fit_width;
  const int new_height = zoomed ? fit_height * kZoomFactor : fit_height;
  const int new_x = std::max(0, (state->view_width - new_width) / 2);
  const int new_y = std::max(0, (state->view_height - new_height) / 2);
  state->zoomed = zoomed;
  lv_obj_scroll_to(pan, 0, 0, LV_ANIM_OFF);
  lv_obj_set_size(image, new_width, new_height);
  // Top-left alignment: with the centre alignment from before, the position
  // would be added to the centring and push the photo off screen.
  lv_obj_align(image, LV_ALIGN_TOP_LEFT, new_x, new_y);
  if (!zoomed) {
    lv_obj_remove_flag(pan, LV_OBJ_FLAG_SCROLLABLE);
    return;
  }
  lv_obj_add_flag(pan, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_update_layout(pan);
  const int tap_x = at != nullptr ? at->x : state->view_width / 2;
  const int tap_y = at != nullptr ? at->y : state->view_height / 2;
  const int image_x = std::clamp(tap_x - old_x, 0, fit_width) * kZoomFactor;
  const int image_y = std::clamp(tap_y - old_y, 0, fit_height) * kZoomFactor;
  const int scroll_x = std::clamp(new_x + image_x - tap_x, 0,
      std::max(0, new_width - state->view_width));
  const int scroll_y = std::clamp(new_y + image_y - tap_y, 0,
      std::max(0, new_height - state->view_height));
  lv_obj_scroll_to(pan, scroll_x, scroll_y, LV_ANIM_OFF);
}

void ViewerDoubleClickedCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->viewer_index < 0 ||
      state->photos[state->viewer_index].video) {
    return;
  }
  lv_point_t point = {};
  lv_indev_t* indev = lv_indev_active();
  if (indev != nullptr) {
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(state->viewer_pan, &area);
    point.x -= area.x1;
    point.y -= area.y1;
  }
  const bool zoom_in = !state->zoomed;
  if (zoom_in && !state->viewer_full_resolution) {
    // The viewer shows the photo decoded for the screen; for zooming in,
    // decode it again with twice as many pixels each way.
    survival::JpegImage sharper;
    const Photo& photo = state->photos[state->viewer_index];
    if (survival::LoadJpegImage(photo.path, static_cast<uint32_t>(state->view_width) * 2,
            static_cast<uint32_t>(state->view_height) * 2, &sharper)) {
      // The tapped point is mapped against the old size, so measure first.
      lv_image_set_src(state->viewer_image, nullptr);
      lv_image_cache_drop(&state->viewer_descriptor);
      survival::FreeJpegImage(&state->viewer_photo);
      state->viewer_photo = sharper;
      FillImageDescriptor(&state->viewer_descriptor, state->viewer_photo);
      lv_image_set_src(state->viewer_image, &state->viewer_descriptor);
    }
    state->viewer_full_resolution = true;
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  PlaceViewerImage(state, zoom_in, indev != nullptr ? &point : nullptr);
}

void CloseDeleteDialog(PhotosViewState* state) {
  if (state->delete_dialog != nullptr) {
    lv_obj_delete_async(state->delete_dialog);
    state->delete_dialog = nullptr;
  }
}

enum class PhotoAction { kTrash, kDeleteForever, kRestore };

void SetSelectMode(PhotosViewState* state, bool on);

/**
 * @brief Reads the list again after files moved. The worker must be stopped.
 */
void ReloadList(PhotosViewState* state) {
  if (state->grid != nullptr) {
    lv_obj_clean(state->grid);
  }
  for (size_t i = 0; i < state->photo_count; ++i) {
    Photo& photo = state->photos[i];
    photo.cell = nullptr;
    photo.image = nullptr;
    photo.check = nullptr;
    photo.selected = false;
  }
  lv_image_cache_drop(nullptr);
  ReleaseThumbnails(state);
  ScanPhotos(state);
  char directory[kMaxPathLength] = {};
  state->trash_count = BuildPhotoDirectoryPath(state, directory, sizeof(directory))
      ? app::CountTrashedPhotos(directory) : 0;
  RebuildGrid(state);
  lv_obj_update_layout(state->root);
  UpdateThumbnails(state);
  StartThumbnailWorker(state);
}

void UpdateHeader(PhotosViewState* state) {
  auto show = [](lv_obj_t* object, bool visible) {
    if (object == nullptr) {
      return;
    }
    if (visible) {
      lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
  };
  if (state->title != nullptr) {
    lv_label_set_text(state->title, state->trash_mode ? "Recently Deleted" : "Photos");
    lv_obj_align(state->title, LV_ALIGN_TOP_LEFT,
        state->trash_mode ? 88 : kSidePadding + 8, kHeaderHeight - 56);
  }
  show(state->header_back, state->trash_mode);
  show(state->trash_note, state->trash_mode);
  show(state->trash_button, !state->trash_mode && !state->select_mode && state->trash_count > 0);
  show(state->select_button, state->photo_count > 0);
  if (state->trash_button_label != nullptr) {
    char text[16];
    std::snprintf(text, sizeof(text), "%d", state->trash_count);
    lv_label_set_text(state->trash_button_label, text);
  }
  if (state->select_button_label != nullptr) {
    lv_label_set_text(state->select_button_label, state->select_mode ? "Cancel" : "Select");
  }
  show(state->action_bar, state->select_mode);
  show(state->action_restore, state->select_mode && state->trash_mode);
  if (state->grid != nullptr) {
    const int top = kHeaderHeight + (state->trash_mode ? kTrashNoteHeight : 0);
    lv_obj_set_pos(state->grid, kSidePadding, top);
    lv_obj_set_height(state->grid,
        state->view_height - top - (state->select_mode ? kActionBarHeight : 0));
  }
}

int SelectedCount(const PhotosViewState* state) {
  int count = 0;
  for (size_t i = 0; i < state->photo_count; ++i) {
    count += state->photos[i].selected ? 1 : 0;
  }
  return count;
}

void UpdateActionBar(PhotosViewState* state) {
  const int count = SelectedCount(state);
  if (state->action_label != nullptr) {
    char text[32];
    std::snprintf(text, sizeof(text), "%d selected", count);
    lv_label_set_text(state->action_label, text);
  }
  if (state->action_all != nullptr) {
    lv_label_set_text(lv_obj_get_child(state->action_all, 0),
        count == static_cast<int>(state->photo_count) ? "None" : "All");
  }
  for (lv_obj_t* button : {state->action_delete, state->action_restore}) {
    if (button == nullptr) {
      continue;
    }
    if (count > 0) {
      lv_obj_remove_state(button, LV_STATE_DISABLED);
    } else {
      lv_obj_add_state(button, LV_STATE_DISABLED);
    }
    lv_obj_set_style_opa(button, count > 0 ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
  }
}

void StyleCheck(lv_obj_t* check, bool selected) {
  lv_obj_set_style_bg_color(check, lv_color_hex(0x0A84FF), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(check, selected ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
  lv_obj_t* mark = lv_obj_get_child(check, 0);
  if (mark != nullptr) {
    if (selected) {
      lv_obj_remove_flag(mark, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(mark, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void CreateCheck(Photo* photo) {
  if (photo->cell == nullptr || photo->check != nullptr) {
    return;
  }
  lv_obj_t* check = lv_obj_create(photo->cell);
  lv_obj_remove_flag(check, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(check, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(check, 44, 44);
  lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -8, 8);
  lv_obj_set_style_radius(check, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_all(check, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(check, 3, LV_PART_MAIN);
  lv_obj_set_style_border_color(check, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_t* mark = CreateLabel(check, icon::kCheck, 0xFFFFFF,
      &lvgl_font_material_symbols_fill_32);
  if (mark != nullptr) {
    lv_obj_center(mark);
  }
  photo->check = check;
  StyleCheck(check, photo->selected);
}

void SetSelectMode(PhotosViewState* state, bool on) {
  state->select_mode = on;
  for (size_t i = 0; i < state->photo_count; ++i) {
    Photo& photo = state->photos[i];
    photo.selected = false;
    if (on) {
      CreateCheck(&photo);
      if (photo.check != nullptr) {
        StyleCheck(photo.check, false);
      }
    } else if (photo.check != nullptr) {
      lv_obj_delete(photo.check);
      photo.check = nullptr;
    }
  }
  UpdateHeader(state);
  UpdateActionBar(state);
}

void ToggleSelection(PhotosViewState* state, int index) {
  if (index < 0 || index >= static_cast<int>(state->photo_count)) {
    return;
  }
  Photo& photo = state->photos[index];
  photo.selected = !photo.selected;
  if (photo.check != nullptr) {
    StyleCheck(photo.check, photo.selected);
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  UpdateActionBar(state);
}

void SetTrashMode(PhotosViewState* state, bool on) {
  if (!StopThumbnailWorker(state)) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }
  state->select_mode = false;
  // Forget the old list's cells before the folder changes under them.
  ReleaseThumbnails(state);
  if (state->grid != nullptr) {
    lv_obj_clean(state->grid);
  }
  for (size_t i = 0; i < state->photo_count; ++i) {
    state->photos[i].cell = nullptr;
    state->photos[i].image = nullptr;
    state->photos[i].check = nullptr;
    state->photos[i].selected = false;
  }
  state->photo_count = 0;
  state->trash_mode = on;
  ReloadList(state);
  if (state->grid != nullptr) {
    lv_obj_scroll_to_y(state->grid, 0, LV_ANIM_OFF);
  }
  UpdateHeader(state);
}

/**
 * @brief Moves, restores or deletes the photo on screen or the selected ones.
 */
void RunPhotoAction(PhotosViewState* state, PhotoAction action, bool selection) {
  const int viewer_index = state->viewer_index;
  StopVideo(state);
  if (!StopThumbnailWorker(state)) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }
  char directory[kMaxPathLength] = {};
  if (!BuildPhotoDirectoryPath(state, directory, sizeof(directory))) {
    StartThumbnailWorker(state);
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }
  int done = 0;
  int failed = 0;
  auto apply = [&](const Photo& photo) {
    const char* name = FileName(photo);
    const bool ok = action == PhotoAction::kTrash ? app::MovePhotoToTrash(directory, name)
        : action == PhotoAction::kRestore ? app::RestorePhotoFromTrash(directory, name)
                                          : app::DeleteTrashedPhoto(directory, name);
    (ok ? done : failed) += 1;
  };
  if (selection) {
    for (size_t i = 0; i < state->photo_count; ++i) {
      if (state->photos[i].selected) {
        apply(state->photos[i]);
      }
    }
  } else if (viewer_index >= 0 && viewer_index < static_cast<int>(state->photo_count)) {
    apply(state->photos[viewer_index]);
  }
  LogMessage(LogLevel::kInfo, __FILE__, __LINE__, "Photos: %d done, %d failed\n", done, failed);
  state->select_mode = false;
  ReloadList(state);
  UpdateHeader(state);
  UpdateActionBar(state);
  PlayUiHapticEffect(failed > 0 ? HapticEffect::kWarning : HapticEffect::kImpactLight);
  if (!selection && viewer_index >= 0) {
    if (state->photo_count == 0) {
      CloseViewer(state);
    } else {
      OpenViewer(state, std::min(viewer_index, static_cast<int>(state->photo_count) - 1));
    }
  }
}

void DeleteDialogButtonCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  lv_obj_t* button = lv_event_get_current_target_obj(event);
  if (state == nullptr || button == nullptr) {
    return;
  }
  const bool confirmed = lv_obj_get_user_data(button) != nullptr;
  const bool selection = state->dialog_for_selection;
  CloseDeleteDialog(state);
  if (confirmed) {
    RunPhotoAction(state, state->trash_mode ? PhotoAction::kDeleteForever : PhotoAction::kTrash,
        selection);
  } else {
    PlayUiHapticEffect(HapticEffect::kSelection);
  }
}

lv_obj_t* DialogButton(lv_obj_t* parent, PhotosViewState* state, const char* text,
    uint32_t color, bool confirm) {
  lv_obj_t* button = lv_button_create(parent);
  lv_obj_set_size(button, 220, 84);
  lv_obj_set_style_radius(button, 42, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_set_user_data(button, confirm ? state : nullptr);
  lv_obj_add_event_cb(button, DeleteDialogButtonCallback, LV_EVENT_CLICKED, state);
  lv_obj_t* label = CreateLabel(button, text, 0xFFFFFF, &lvgl_font_google_sans_flex_28);
  if (label != nullptr) {
    lv_obj_center(label);
  }
  return button;
}

/**
 * @brief Asks before deleting: outside the trash the photos can still come
 *        back, inside it they are gone for good.
 */
void ShowDeleteDialog(PhotosViewState* state, bool selection) {
  if (state->delete_dialog != nullptr) {
    return;
  }
  const int count = selection ? SelectedCount(state) : 1;
  if (count == 0) {
    return;
  }
  state->dialog_for_selection = selection;
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  // A scrim over everything takes every touch while the question is open.
  lv_obj_t* scrim = lv_obj_create(state->root);
  state->delete_dialog = scrim;
  lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(scrim, state->view_width, state->view_height);
  lv_obj_set_pos(scrim, 0, 0);
  lv_obj_set_style_radius(scrim, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(scrim, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(scrim, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scrim, LV_OPA_60, LV_PART_MAIN);

  lv_obj_t* card = lv_obj_create(scrim);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(card, state->view_width - 64, LV_SIZE_CONTENT);
  lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(card, 32, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1E), LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 28, LV_PART_MAIN);
  lv_obj_set_style_pad_row(card, 18, LV_PART_MAIN);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  char title[48];
  if (count > 1) {
    std::snprintf(title, sizeof(title), "Delete %d items%s", count,
        state->trash_mode ? " forever?" : "?");
  } else {
    std::snprintf(title, sizeof(title), "%s",
        state->trash_mode ? "Delete forever?" : "Delete this photo?");
  }
  CreateLabel(card, title, 0xFFFFFF, &lvgl_font_google_sans_flex_32);
  lv_obj_t* note = CreateLabel(card,
      state->trash_mode ? "This cannot be undone."
      : count > 1       ? "They move to Recently Deleted."
                        : "It moves to Recently Deleted.",
      0xAEAEB2, &lvgl_font_google_sans_flex_24);
  if (note != nullptr) {
    lv_obj_set_width(note, state->view_width - 120);
    lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(note, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  }
  lv_obj_t* buttons = lv_obj_create(card);
  lv_obj_remove_flag(buttons, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(buttons, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(buttons, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(buttons, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(buttons, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_top(buttons, 10, LV_PART_MAIN);
  lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
      LV_FLEX_ALIGN_CENTER);
  DialogButton(buttons, state, "Cancel", 0x3A3A3C, false);
  DialogButton(buttons, state, "Delete", 0xD7362B, true);
}

void ViewerDeleteClickedCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  if (state != nullptr && state->viewer_index >= 0) {
    ShowDeleteDialog(state, false);
  }
}

void ViewerRestoreClickedCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  if (state != nullptr && state->viewer_index >= 0) {
    RunPhotoAction(state, PhotoAction::kRestore, false);
  }
}

void SelectButtonCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  PlayUiHapticEffect(HapticEffect::kSelection);
  SetSelectMode(state, !state->select_mode);
}

void TrashButtonCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  SetTrashMode(state, true);
}

void HeaderBackCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  SetTrashMode(state, false);
}

void ActionAllCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  const bool select = SelectedCount(state) != static_cast<int>(state->photo_count);
  for (size_t i = 0; i < state->photo_count; ++i) {
    state->photos[i].selected = select;
    if (state->photos[i].check != nullptr) {
      StyleCheck(state->photos[i].check, select);
    }
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  UpdateActionBar(state);
}

void ActionDeleteCallback(lv_event_t* event) {
  ShowDeleteDialog(static_cast<PhotosViewState*>(lv_event_get_user_data(event)), true);
}

void ActionRestoreCallback(lv_event_t* event) {
  auto* state = static_cast<PhotosViewState*>(lv_event_get_user_data(event));
  if (SelectedCount(state) > 0) {
    RunPhotoAction(state, PhotoAction::kRestore, true);
  }
}

lv_obj_t* PillButton(lv_obj_t* parent, const char* text, uint32_t color, int width, int height,
    lv_event_cb_t callback, PhotosViewState* state) {
  lv_obj_t* button = lv_button_create(parent);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, state);
  lv_obj_t* label = CreateLabel(button, text, 0xFFFFFF, &lvgl_font_google_sans_flex_24);
  if (label != nullptr) {
    lv_obj_center(label);
  }
  return button;
}

void CreatePhotoControls(PhotosViewState* state, lv_obj_t* container, int width, int height) {
  const uint32_t chip = theme::ActiveThemeColors().surface_container_high;
  const uint32_t text = theme::ActiveThemeColors().on_surface;
  state->select_button = PillButton(container, "Select", chip, 118, 56, SelectButtonCallback, state);
  lv_obj_align(state->select_button, LV_ALIGN_TOP_RIGHT, -kSidePadding, kHeaderHeight - 62);
  state->select_button_label = lv_obj_get_child(state->select_button, 0);
  lv_obj_set_style_text_color(state->select_button_label, lv_color_hex(text), LV_PART_MAIN);

  // Recently Deleted: a bin and how many wait in it.
  state->trash_button = PillButton(container, icon::kDelete, chip, 112, 56, TrashButtonCallback, state);
  lv_obj_align(state->trash_button, LV_ALIGN_TOP_RIGHT, -kSidePadding - 118 - 12, kHeaderHeight - 62);
  lv_obj_t* bin = lv_obj_get_child(state->trash_button, 0);
  lv_obj_set_style_text_font(bin, &lvgl_font_material_symbols_fill_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(bin, lv_color_hex(text), LV_PART_MAIN);
  lv_obj_align(bin, LV_ALIGN_LEFT_MID, 8, 0);
  state->trash_button_label = CreateLabel(state->trash_button, "0", text,
      &lvgl_font_google_sans_flex_24);
  lv_obj_align(state->trash_button_label, LV_ALIGN_RIGHT_MID, -8, 0);

  state->header_back = lv_button_create(container);
  lv_obj_set_size(state->header_back, 68, 68);
  lv_obj_align(state->header_back, LV_ALIGN_TOP_LEFT, 12, kHeaderHeight - 68);
  lv_obj_set_style_radius(state->header_back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->header_back, lv_color_hex(chip), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(state->header_back, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(state->header_back, HeaderBackCallback, LV_EVENT_CLICKED, state);
  lv_obj_t* arrow = CreateLabel(state->header_back, icon::kArrowBack, text,
      &lvgl_font_material_symbols_fill_32);
  lv_obj_center(arrow);

  state->trash_note = CreateLabel(container,
      "Deleted photos stay here until you delete them, or until the storage "
      "runs low.",
      theme::ActiveThemeColors().on_surface_variant, &lvgl_font_google_sans_flex_22);
  lv_obj_set_width(state->trash_note, width - 2 * kSidePadding - 8);
  lv_label_set_long_mode(state->trash_note, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(state->trash_note, kSidePadding + 8, kHeaderHeight);

  state->action_bar = lv_obj_create(container);
  lv_obj_remove_flag(state->action_bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(state->action_bar, width, kActionBarHeight);
  lv_obj_set_pos(state->action_bar, 0, height - kActionBarHeight);
  lv_obj_set_style_radius(state->action_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(state->action_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(state->action_bar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(state->action_bar,
      lv_color_hex(theme::ActiveThemeColors().surface_container), LV_PART_MAIN);
  state->action_label = CreateLabel(state->action_bar, "0 selected", text,
      &lvgl_font_google_sans_flex_22);
  lv_obj_align(state->action_label, LV_ALIGN_TOP_MID, 0, 6);
  state->action_all = PillButton(state->action_bar, "All", chip, 110, 68, ActionAllCallback, state);
  lv_obj_align(state->action_all, LV_ALIGN_TOP_LEFT, 18, 42);
  lv_obj_set_style_text_color(lv_obj_get_child(state->action_all, 0), lv_color_hex(text), LV_PART_MAIN);
  state->action_restore = PillButton(state->action_bar, "Restore", 0x0A84FF, 170, 68,
      ActionRestoreCallback, state);
  lv_obj_align(state->action_restore, LV_ALIGN_TOP_MID, 0, 42);
  state->action_delete = PillButton(state->action_bar, "Delete", 0xD7362B, 170, 68,
      ActionDeleteCallback, state);
  lv_obj_align(state->action_delete, LV_ALIGN_TOP_RIGHT, -18, 42);
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
        state->trash_mode ? "Nothing here."
        : card_mounted || app::GetEdition() == app::Edition::kMini
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
    // Square cells: rounded clipping made LVGL draw every visible cell
    // through an extra layer, which is what made scrolling stutter.
    lv_obj_set_style_radius(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(cell, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(cell,
        lv_color_hex(theme::ActiveThemeColors().surface_container_high),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_user_data(
        cell, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(cell, CellClickedCallback, LV_EVENT_CLICKED, state);
    photo.cell = cell;
    photo.image = nullptr;

    if (photo.thumbnail_state.load() == kThumbnailReady) {
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
 * @brief Tells the worker what is on screen, hangs finished thumbnails into
 *        their cells and drops those far out of view.
 * @param timer LVGL timer.
 */
void UpdateThumbnails(PhotosViewState* state) {
  if (state == nullptr || state->grid == nullptr || state->photo_count == 0) {
    return;
  }
  const int row_height = state->thumbnail_size + kGridGap;
  const int scroll = std::max<int>(0, lv_obj_get_scroll_y(state->grid));
  const int first_row = scroll / row_height;
  const int visible_rows = lv_obj_get_height(state->grid) / row_height + 2;
  const int count = static_cast<int>(state->photo_count);
  state->visible_first.store(first_row * kGridColumns);
  state->visible_last.store(
      std::min(count, (first_row + visible_rows) * kGridColumns) - 1);
  const int keep_first =
      std::max(0, (first_row - kKeepRowsBehind) * kGridColumns);
  const int keep_last = std::min(count,
      (first_row + visible_rows + kKeepRowsAhead) * kGridColumns) - 1;
  state->keep_first.store(keep_first);
  state->keep_last.store(keep_last);

  for (int index = 0; index < count; ++index) {
    Photo& photo = state->photos[index];
    if (photo.thumbnail_state.load() != kThumbnailReady) {
      continue;
    }
    if (index < keep_first || index > keep_last) {
      if (photo.image != nullptr) {
        lv_obj_delete(photo.image);
        photo.image = nullptr;
      }
      lv_image_cache_drop(&photo.thumbnail_descriptor);
      survival::FreeJpegImage(&photo.thumbnail);
      photo.thumbnail_state.store(kThumbnailNone);
      continue;
    }
    if (photo.cell != nullptr && photo.image == nullptr) {
      lv_image_cache_drop(&photo.thumbnail_descriptor);
      FillImageDescriptor(&photo.thumbnail_descriptor, photo.thumbnail);
      lv_obj_t* image = lv_image_create(photo.cell);
      if (image != nullptr) {
        lv_image_set_src(image, &photo.thumbnail_descriptor);
        lv_obj_center(image);
        lv_obj_move_to_index(image, 0);
        photo.image = image;
      }
    }
  }
}

void ThumbnailTimerCallback(lv_timer_t* timer) {
  UpdateThumbnails(static_cast<PhotosViewState*>(lv_timer_get_user_data(timer)));
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
  if (!StopThumbnailWorker(state)) {
    // The worker still holds the state; leaking it beats a crash.
    return;
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
  char photo_directory[kMaxPathLength] = {};
  state->trash_count = BuildPhotoDirectoryPath(state, photo_directory, sizeof(photo_directory))
      ? app::CountTrashedPhotos(photo_directory) : 0;
  CreatePhotoControls(state, container, config.width, config.height);
  UpdateHeader(state);
  UpdateActionBar(state);
  app::RequestPhotoTrashPurge(state->storage);
  RegisterConditionalBackNavigationHandler(container, [state]() {
    if (state->delete_dialog != nullptr) {
      CloseDeleteDialog(state);
      return true;
    }
    if (state->select_mode) {
      SetSelectMode(state, false);
      return true;
    }
    if (state->trash_mode) {
      SetTrashMode(state, false);
      return true;
    }
    return false;
  });

  // Runs with an empty list too: the list can fill when the view changes.
  state->thumbnail_timer =
      lv_timer_create(ThumbnailTimerCallback, kThumbnailTimerMs, state);
  if (state->thumbnail_timer == nullptr) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Create thumbnail timer failed\n");
  }
  if (state->photo_count > 0) {
    // Publish the visible range before the worker starts picking.
    lv_obj_update_layout(container);
    UpdateThumbnails(state);
    StartThumbnailWorker(state);
  }
  return container;
}

}  // namespace lilygo_box::ui
