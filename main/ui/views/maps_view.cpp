/*
 * @Description: Offline maps app view
 * @License: GPL 3.0
 */
#include "ui/views/maps_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "app/storage/map_storage.h"
#include "base/logger.h"
#include "hal/providers/gps_provider.h"
#include "hal/providers/imu_provider.h"
#include "hal/providers/storage_provider.h"
#include "survival/map_geometry.h"
#include "survival/map_source.h"
#include "ui/haptic_feedback.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"

namespace lilygo_box::ui {
namespace {

constexpr const char* kMapDirectory = "maps";
constexpr size_t kMaxPathLength = 192;
constexpr int kTilePixels = static_cast<int>(survival::kMapTilePixels);
// Enough to cover the screen plus one tile of margin in every direction.
constexpr int kMaxVisibleTiles = 24;

constexpr uint32_t kTileTimerMs = 30;
constexpr uint32_t kGpsTimerMs = 1000;
constexpr uint8_t kMinZoom = 2;

constexpr int kButtonSize = 72;
constexpr int kButtonMargin = 18;
constexpr int kStatusHeight = 64;
constexpr int kMarkerSize = 22;

constexpr uint32_t kMarkerColor = 0x2F7BE8;
constexpr uint32_t kMarkerStaleColor = 0x8A8A8E;

// One LVGL image standing in for a map tile on screen.
struct TileSlot {
  lv_obj_t* image = nullptr;
  lv_image_dsc_t descriptor = {};
  uint8_t zoom = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  bool occupied = false;
};

struct MapsViewState {
  hal::StorageProvider* storage = nullptr;
  hal::GpsProvider* gps = nullptr;
  hal::ImuProvider* imu = nullptr;

  lv_obj_t* root = nullptr;
  lv_obj_t* canvas = nullptr;
  lv_obj_t* marker = nullptr;
  lv_obj_t* marker_dot = nullptr;
  lv_obj_t* status_label = nullptr;
  lv_obj_t* status_bar = nullptr;
  lv_timer_t* tile_timer = nullptr;
  lv_timer_t* gps_timer = nullptr;

  survival::MapSource source;
  TileSlot slots[kMaxVisibleTiles];

  int view_width = 0;
  int view_height = 0;

  // Viewport centre in world pixels at the current zoom, where one tile is
  // kTilePixels across. Keeping it in pixels rather than degrees makes panning
  // exact and avoids reprojecting on every drag event.
  double centre_pixel_x = 0.0;
  double centre_pixel_y = 0.0;
  uint8_t zoom = 13;

  bool dragging = false;
  lv_point_t drag_origin = {};
  double drag_start_x = 0.0;
  double drag_start_y = 0.0;

  bool has_fix = false;
  double fix_latitude = 0.0;
  double fix_longitude = 0.0;
  uint8_t satellites = 0;
  bool follow_fix = true;
  lv_obj_t* trails_button = nullptr;
  char status_text[96] = {};
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

// The projection lives in survival/map_geometry.h so the host tests exercise
// exactly the same maths the app does.
using survival::LatitudeToPixelY;
using survival::LongitudeToPixelX;
using survival::MapWorldSpan;
using survival::PixelXToLongitude;
using survival::PixelYToLatitude;

/**
 * @brief Turns an NMEA degrees-and-minutes coordinate into decimal degrees.
 * @param coordinate Coordinate from the GPS provider.
 * @return Decimal degrees, negative for south and west.
 */
double CoordinateToDegrees(const hal::GpsCoordinate& coordinate) {
  if (!coordinate.ready) {
    return 0.0;
  }
  double degrees = static_cast<double>(coordinate.degrees) +
      static_cast<double>(coordinate.minutes) / 60.0;
  if (coordinate.direction[0] == 'S' || coordinate.direction[0] == 'W') {
    degrees = -degrees;
  }
  return degrees;
}

/**
 * @brief Builds the path to the map directory on the card.
 * @param state View state.
 * @param buffer Output buffer.
 * @param size Buffer size.
 * @return true when the SD card is mounted and the path was built.
 */
bool BuildDirectoryPath(
    const MapsViewState* state, char* buffer, size_t size) {
  if (state->storage == nullptr || !state->storage->IsSdCardMounted()) {
    return false;
  }
  const char* base = state->storage->SdCardBasePath();
  if (base == nullptr || base[0] == '\0') {
    return false;
  }
  return std::snprintf(buffer, size, "%s/%s", base, kMapDirectory) <
      static_cast<int>(size);
}

/**
 * @brief Refreshes the status line under the map.
 * @param state View state.
 */
void UpdateStatus(MapsViewState* state) {
  if (state->status_label == nullptr) {
    return;
  }
  const double latitude =
      PixelYToLatitude(state->centre_pixel_y, state->zoom);
  const double longitude =
      PixelXToLongitude(state->centre_pixel_x, state->zoom);

  if (state->has_fix) {
    std::snprintf(state->status_text, sizeof(state->status_text),
        "z%u   %.5f, %.5f   %u sat", static_cast<unsigned int>(state->zoom),
        state->fix_latitude, state->fix_longitude,
        static_cast<unsigned int>(state->satellites));
  } else if (state->gps != nullptr) {
    std::snprintf(state->status_text, sizeof(state->status_text),
        "z%u   %.4f, %.4f   searching (%u sat)",
        static_cast<unsigned int>(state->zoom), latitude, longitude,
        static_cast<unsigned int>(state->satellites));
  } else {
    std::snprintf(state->status_text, sizeof(state->status_text),
        "z%u   %.4f, %.4f", static_cast<unsigned int>(state->zoom), latitude,
        longitude);
  }
  lv_label_set_text(state->status_label, state->status_text);
}

/**
 * @brief Moves the position marker to where the fix falls on screen.
 * @param state View state.
 */
void UpdateMarker(MapsViewState* state) {
  if (state->marker == nullptr) {
    return;
  }
  if (!state->has_fix) {
    lv_obj_add_flag(state->marker, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  const double pixel_x =
      LongitudeToPixelX(state->fix_longitude, state->zoom);
  const double pixel_y = LatitudeToPixelY(state->fix_latitude, state->zoom);
  const int screen_x = static_cast<int>(
      std::lround(pixel_x - state->centre_pixel_x)) + state->view_width / 2;
  const int screen_y = static_cast<int>(
      std::lround(pixel_y - state->centre_pixel_y)) + state->view_height / 2;

  if (screen_x < -kMarkerSize || screen_y < -kMarkerSize ||
      screen_x > state->view_width + kMarkerSize ||
      screen_y > state->view_height + kMarkerSize) {
    lv_obj_add_flag(state->marker, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_remove_flag(state->marker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(state->marker, screen_x - kMarkerSize / 2,
      screen_y - kMarkerSize / 2);
  lv_obj_move_to_index(state->marker, -1);
}

/**
 * @brief Places the tiles that cover the viewport, drawing what is missing.
 *
 * Only one uncached tile is rendered per call so the UI keeps responding while
 * the map fills in, the way a phone map does.
 *
 * @param state View state.
 * @param budget How many tiles may be rendered this call.
 */
void RefreshTiles(MapsViewState* state, int budget) {
  if (state->canvas == nullptr) {
    return;
  }

  const int half_width = state->view_width / 2;
  const int half_height = state->view_height / 2;
  const double left = state->centre_pixel_x - half_width;
  const double top = state->centre_pixel_y - half_height;
  const int32_t tile_count = 1 << state->zoom;

  const int first_x = static_cast<int>(std::floor(left / kTilePixels));
  const int first_y = static_cast<int>(std::floor(top / kTilePixels));
  const int columns =
      static_cast<int>(state->view_width / kTilePixels) + 2;
  const int rows = static_cast<int>(state->view_height / kTilePixels) + 2;

  // Mark every slot free, then claim the ones still on screen.
  bool used[kMaxVisibleTiles] = {};
  int slot_index = 0;

  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < columns; ++column) {
      if (slot_index >= kMaxVisibleTiles) {
        break;
      }
      const int tile_x = first_x + column;
      const int tile_y = first_y + row;
      if (tile_y < 0 || tile_y >= tile_count) {
        continue;
      }
      // Longitude wraps, so a column off one edge comes back on the other.
      const uint32_t wrapped_x = static_cast<uint32_t>(
          ((tile_x % tile_count) + tile_count) % tile_count);

      TileSlot& slot = state->slots[slot_index];
      const int screen_x = static_cast<int>(
          std::lround(static_cast<double>(tile_x) * kTilePixels - left));
      const int screen_y = static_cast<int>(
          std::lround(static_cast<double>(tile_y) * kTilePixels - top));

      const bool same = slot.occupied && slot.zoom == state->zoom &&
          slot.x == wrapped_x && slot.y == static_cast<uint32_t>(tile_y);
      if (!same) {
        const bool cached = state->source.IsCached(
            state->zoom, wrapped_x, static_cast<uint32_t>(tile_y));
        if (!cached && budget <= 0) {
          // Leave the slot showing whatever it had; it will be filled on a
          // later tick rather than flashing empty.
          ++slot_index;
          continue;
        }
        if (!cached) {
          --budget;
        }
        bool approximate = false;
        const uint16_t* pixels = state->source.GetTile(state->zoom, wrapped_x,
            static_cast<uint32_t>(tile_y), &approximate);
        if (pixels == nullptr) {
          ++slot_index;
          continue;
        }
        std::memset(&slot.descriptor, 0, sizeof(slot.descriptor));
        slot.descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
        slot.descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
        slot.descriptor.header.w = survival::kMapTilePixels;
        slot.descriptor.header.h = survival::kMapTilePixels;
        slot.descriptor.header.stride =
            survival::kMapTilePixels * sizeof(uint16_t);
        slot.descriptor.data = reinterpret_cast<const uint8_t*>(pixels);
        slot.descriptor.data_size = survival::kMapTilePixels *
            survival::kMapTilePixels * sizeof(uint16_t);
        slot.zoom = state->zoom;
        slot.x = wrapped_x;
        slot.y = static_cast<uint32_t>(tile_y);
        slot.occupied = true;

        if (slot.image == nullptr) {
          slot.image = lv_image_create(state->canvas);
          if (slot.image == nullptr) {
            continue;
          }
          lv_obj_remove_flag(slot.image, LV_OBJ_FLAG_CLICKABLE);
        }
        lv_image_set_src(slot.image, &slot.descriptor);
      }

      if (slot.image != nullptr) {
        lv_obj_remove_flag(slot.image, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(slot.image, screen_x, screen_y);
      }
      used[slot_index] = true;
      ++slot_index;
    }
  }

  for (int index = 0; index < kMaxVisibleTiles; ++index) {
    if (!used[index] && state->slots[index].image != nullptr) {
      lv_obj_add_flag(state->slots[index].image, LV_OBJ_FLAG_HIDDEN);
      state->slots[index].occupied = false;
    }
  }

  UpdateMarker(state);
}

/**
 * @brief Saves where the map is looking so it opens there next time.
 * @param state View state.
 */
void SaveViewport(MapsViewState* state) {
  app::MapViewport viewport;
  viewport.latitude = PixelYToLatitude(state->centre_pixel_y, state->zoom);
  viewport.longitude = PixelXToLongitude(state->centre_pixel_x, state->zoom);
  viewport.zoom = state->zoom;
  viewport.valid = true;
  app::UpdateMapViewport(viewport);
}

/**
 * @brief Changes zoom while keeping the centre of the screen fixed.
 * @param state View state.
 * @param delta Levels to zoom by.
 */
void ChangeZoom(MapsViewState* state, int delta) {
  const uint8_t highest = std::max<uint8_t>(state->source.max_zoom(), kMinZoom);
  const int target = std::clamp(static_cast<int>(state->zoom) + delta,
      static_cast<int>(kMinZoom), static_cast<int>(highest));
  if (target == state->zoom) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }

  const double latitude =
      PixelYToLatitude(state->centre_pixel_y, state->zoom);
  const double longitude =
      PixelXToLongitude(state->centre_pixel_x, state->zoom);
  state->zoom = static_cast<uint8_t>(target);
  state->centre_pixel_x = LongitudeToPixelX(longitude, state->zoom);
  state->centre_pixel_y = LatitudeToPixelY(latitude, state->zoom);

  // Tiles from the old zoom are meaningless now.
  for (TileSlot& slot : state->slots) {
    slot.occupied = false;
    if (slot.image != nullptr) {
      lv_obj_add_flag(slot.image, LV_OBJ_FLAG_HIDDEN);
    }
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  RefreshTiles(state, 4);
  UpdateStatus(state);
  SaveViewport(state);
}

/**
 * @brief Handles zoom button taps.
 * @param event LVGL event.
 */
void ZoomClickedCallback(lv_event_t* event) {
  auto* state = static_cast<MapsViewState*>(lv_event_get_user_data(event));
  lv_obj_t* button = lv_event_get_target_obj(event);
  if (state == nullptr || button == nullptr) {
    return;
  }
  const int delta =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
  ChangeZoom(state, delta);
}

/**
 * @brief Paints the trail button to show whether the overlay is on.
 * @param state View state.
 */
void ApplyTrailsButtonState(MapsViewState* state) {
  if (state->trails_button == nullptr) {
    return;
  }
  const bool active =
      state->source.has_trails() && state->source.trails_enabled();
  lv_obj_set_style_bg_color(state->trails_button,
      lv_color_hex(active
              ? 0x1E8E3E
              : theme::ActiveThemeColors().surface_container_highest),
      LV_PART_MAIN);
  lv_obj_t* label = lv_obj_get_child(state->trails_button, 0);
  if (label != nullptr) {
    lv_obj_set_style_text_color(label,
        lv_color_hex(active ? 0xFFFFFF
                            : theme::ActiveThemeColors().on_surface),
        LV_PART_MAIN);
  }
}

/**
 * @brief Turns the marked-trail overlay on and off.
 * @param event LVGL event.
 */
void TrailsClickedCallback(lv_event_t* event) {
  auto* state = static_cast<MapsViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  if (!state->source.has_trails()) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    if (state->status_label != nullptr) {
      lv_label_set_text(state->status_label,
          "No trail overlay. Copy a .svtrails file into /maps.");
    }
    return;
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  state->source.SetTrailsEnabled(!state->source.trails_enabled());
  ApplyTrailsButtonState(state);
  for (TileSlot& slot : state->slots) {
    slot.occupied = false;
    if (slot.image != nullptr) {
      lv_obj_add_flag(slot.image, LV_OBJ_FLAG_HIDDEN);
    }
  }
  RefreshTiles(state, 4);
}

/**
 * @brief Recentres the map on the current fix.
 * @param event LVGL event.
 */
void LocateClickedCallback(lv_event_t* event) {
  auto* state = static_cast<MapsViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  if (!state->has_fix) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }
  PlayUiHapticEffect(HapticEffect::kImpactMedium);
  state->follow_fix = true;
  state->centre_pixel_x =
      LongitudeToPixelX(state->fix_longitude, state->zoom);
  state->centre_pixel_y = LatitudeToPixelY(state->fix_latitude, state->zoom);
  RefreshTiles(state, 4);
  UpdateStatus(state);
  SaveViewport(state);
}

/**
 * @brief Drags the map under the finger.
 * @param event LVGL event.
 */
void CanvasPressedCallback(lv_event_t* event) {
  auto* state = static_cast<MapsViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  lv_indev_t* input = lv_indev_active();
  if (input == nullptr) {
    return;
  }
  lv_point_t point = {};
  lv_indev_get_point(input, &point);

  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED) {
    state->dragging = true;
    state->drag_origin = point;
    state->drag_start_x = state->centre_pixel_x;
    state->drag_start_y = state->centre_pixel_y;
    return;
  }
  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    if (state->dragging) {
      state->dragging = false;
      SaveViewport(state);
    }
    return;
  }
  if (code != LV_EVENT_PRESSING || !state->dragging) {
    return;
  }

  // Dragging always means the user wants to look somewhere other than the fix.
  const int moved_x = point.x - state->drag_origin.x;
  const int moved_y = point.y - state->drag_origin.y;
  if (std::abs(moved_x) > 6 || std::abs(moved_y) > 6) {
    state->follow_fix = false;
  }
  state->centre_pixel_x = state->drag_start_x - moved_x;
  state->centre_pixel_y = std::clamp(
      state->drag_start_y - moved_y, 0.0, MapWorldSpan(state->zoom));
  RefreshTiles(state, 1);
  UpdateStatus(state);
}

/**
 * @brief Keeps filling in tiles that are still missing.
 * @param timer LVGL timer.
 */
void TileTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<MapsViewState*>(lv_timer_get_user_data(timer));
  if (state != nullptr && !state->dragging) {
    RefreshTiles(state, 1);
  }
}

/**
 * @brief Polls the GPS and moves the marker.
 * @param timer LVGL timer.
 */
void GpsTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<MapsViewState*>(lv_timer_get_user_data(timer));
  if (state == nullptr || state->gps == nullptr) {
    return;
  }

  hal::GpsStatus status;
  if (!state->gps->ReadGpsStatus(&status)) {
    return;
  }
  state->satellites = status.satellites_used_ready
      ? status.satellites_used
      : status.satellites_in_view;

  const bool had_fix = state->has_fix;
  state->has_fix = status.positioned && status.latitude.ready &&
      status.longitude.ready;
  if (state->has_fix) {
    state->fix_latitude = CoordinateToDegrees(status.latitude);
    state->fix_longitude = CoordinateToDegrees(status.longitude);
    if (!had_fix) {
      // The first fix is worth announcing, and worth jumping to.
      PlayUiHapticEffect(HapticEffect::kSuccess);
      state->follow_fix = true;
    }
    if (state->follow_fix) {
      state->centre_pixel_x =
          LongitudeToPixelX(state->fix_longitude, state->zoom);
      state->centre_pixel_y =
          LatitudeToPixelY(state->fix_latitude, state->zoom);
      RefreshTiles(state, 2);
    }
  }
  if (state->marker_dot != nullptr) {
    lv_obj_set_style_bg_color(state->marker_dot,
        lv_color_hex(state->has_fix ? kMarkerColor : kMarkerStaleColor),
        LV_PART_MAIN);
  }
  UpdateMarker(state);
  UpdateStatus(state);
}

/**
 * @brief Creates one round control button.
 * @param state View state.
 * @param symbol Icon glyph.
 * @param y Vertical position from the bottom of the view.
 * @param payload Value stored on the button for its handler.
 * @param handler Click handler.
 * @return The button, or nullptr on failure.
 */
lv_obj_t* CreateControl(MapsViewState* state, const char* symbol, int y,
    intptr_t payload, lv_event_cb_t handler) {
  lv_obj_t* button = lv_button_create(state->root);
  if (button == nullptr) {
    return nullptr;
  }
  lv_obj_set_size(button, kButtonSize, kButtonSize);
  lv_obj_align(button, LV_ALIGN_BOTTOM_RIGHT, -kButtonMargin, -y);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button,
      lv_color_hex(theme::ActiveThemeColors().surface_container_highest),
      LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, 235, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 8, LV_PART_MAIN);
  lv_obj_set_style_shadow_opa(button, 60, LV_PART_MAIN);
  lv_obj_set_user_data(button, reinterpret_cast<void*>(payload));
  lv_obj_add_event_cb(button, handler, LV_EVENT_CLICKED, state);

  lv_obj_t* label = lv_label_create(button);
  if (label != nullptr) {
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_color(label,
        lv_color_hex(theme::ActiveThemeColors().on_surface), LV_PART_MAIN);
    lv_obj_set_style_text_font(
        label, &lvgl_font_material_symbols_fill_32, LV_PART_MAIN);
    lv_obj_center(label);
  }
  return button;
}

/**
 * @brief Frees everything the view owns.
 * @param event LVGL event.
 */
void ViewDeletedCallback(lv_event_t* event) {
  auto* state = static_cast<MapsViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  if (state->tile_timer != nullptr) {
    lv_timer_delete(state->tile_timer);
  }
  if (state->gps_timer != nullptr) {
    lv_timer_delete(state->gps_timer);
  }
  if (state->gps != nullptr) {
    state->gps->SetGpsEnabled(false);
  }
  state->source.Close();
  delete state;
}

}  // namespace

lv_obj_t* CreateMapsView(lv_obj_t* parent, const app::AppEntry& app_entry,
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
  lv_obj_set_style_bg_color(container, lv_color_hex(0xE4DFD5), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container, LV_OPA_COVER, LV_PART_MAIN);

  auto* state = new (std::nothrow) MapsViewState();
  if (state == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  state->root = container;
  state->storage = config.storage;
  state->gps = config.gps;
  state->imu = config.imu;
  state->view_width = config.width;
  state->view_height = config.height;
  lv_obj_add_event_cb(container, ViewDeletedCallback, LV_EVENT_DELETE, state);

  state->canvas = lv_obj_create(container);
  if (state->canvas == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  MakeTransparent(state->canvas);
  lv_obj_remove_flag(state->canvas, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(state->canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_clip_corner(state->canvas, true, LV_PART_MAIN);
  lv_obj_set_size(state->canvas, config.width, config.height);
  lv_obj_set_pos(state->canvas, 0, 0);
  for (lv_event_code_t code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING,
           LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
    lv_obj_add_event_cb(state->canvas, CanvasPressedCallback, code, state);
  }

  // The marker is a ring with a filled dot, which reads as a position rather
  // than as part of the map.
  state->marker = lv_obj_create(container);
  if (state->marker != nullptr) {
    lv_obj_remove_flag(state->marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state->marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(state->marker, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(state->marker, kMarkerSize, kMarkerSize);
    lv_obj_set_style_radius(state->marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->marker, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->marker, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->marker, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(state->marker, 8, LV_PART_MAIN);
    lv_obj_set_style_shadow_opa(state->marker, 90, LV_PART_MAIN);

    state->marker_dot = lv_obj_create(state->marker);
    if (state->marker_dot != nullptr) {
      lv_obj_remove_flag(state->marker_dot, LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_remove_flag(state->marker_dot, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_size(state->marker_dot, kMarkerSize - 8, kMarkerSize - 8);
      lv_obj_set_style_radius(
          state->marker_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
      lv_obj_set_style_border_width(state->marker_dot, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(state->marker_dot, 0, LV_PART_MAIN);
      lv_obj_set_style_bg_color(
          state->marker_dot, lv_color_hex(kMarkerStaleColor), LV_PART_MAIN);
      lv_obj_set_style_bg_opa(state->marker_dot, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_center(state->marker_dot);
    }
  }

  state->status_bar = lv_obj_create(container);
  if (state->status_bar != nullptr) {
    lv_obj_remove_flag(state->status_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(state->status_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(state->status_bar, config.width, kStatusHeight);
    lv_obj_align(state->status_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(state->status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(state->status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(state->status_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->status_bar, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(state->status_bar, 140, LV_PART_MAIN);

    state->status_label = lv_label_create(state->status_bar);
    if (state->status_label != nullptr) {
      lv_obj_set_style_text_color(
          state->status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
      lv_obj_set_style_text_font(
          state->status_label, &lvgl_font_google_sans_flex_22, LV_PART_MAIN);
      lv_obj_align(state->status_label, LV_ALIGN_LEFT_MID, 18, 0);
    }
  }

  CreateControl(state, icon::kAdd,
      kStatusHeight + kButtonMargin + 2 * (kButtonSize + 12), 1,
      ZoomClickedCallback);
  CreateControl(state, icon::kRemove,
      kStatusHeight + kButtonMargin + kButtonSize + 12, -1,
      ZoomClickedCallback);
  CreateControl(state, icon::kMyLocation, kStatusHeight + kButtonMargin, 0,
      LocateClickedCallback);
  state->trails_button = CreateControl(state, icon::kRoute,
      kStatusHeight + kButtonMargin + 3 * (kButtonSize + 12), 0,
      TrailsClickedCallback);
  ApplyTrailsButtonState(state);

  if (config.set_status_bar_text_color) {
    config.set_status_bar_text_color(0x101010);
  }

  if (state->storage != nullptr) {
    state->storage->EnsureSdCardMounted();
  }

  // Start where the map was left, or in the middle of the country the archives
  // most likely cover.
  const app::MapViewport viewport = app::GetMapViewport();
  state->zoom = viewport.valid ? viewport.zoom : 13;
  const double latitude = viewport.valid ? viewport.latitude : 49.8;
  const double longitude = viewport.valid ? viewport.longitude : 15.47;
  state->centre_pixel_x = LongitudeToPixelX(longitude, state->zoom);
  state->centre_pixel_y = LatitudeToPixelY(latitude, state->zoom);
  state->follow_fix = true;

  char directory[kMaxPathLength] = {};
  if (BuildDirectoryPath(state, directory, sizeof(directory)) &&
      state->source.OpenDirectory(directory) > 0) {
    state->zoom = std::min(state->zoom, state->source.max_zoom());
    ApplyTrailsButtonState(state);
    RefreshTiles(state, 6);
  } else if (state->status_label != nullptr) {
    const bool card_mounted =
        state->storage != nullptr && state->storage->IsSdCardMounted();
    lv_label_set_text(state->status_label,
        card_mounted
            ? "SD card OK, but no maps yet. Copy .pmtiles files from the "
              "SurvivalOS web page anywhere on the card."
            : "SD card not detected (needs FAT32 or exFAT).");
  }
  UpdateStatus(state);

  if (state->gps != nullptr) {
    state->gps->SetGpsEnabled(true);
  }
  state->tile_timer = lv_timer_create(TileTimerCallback, kTileTimerMs, state);
  state->gps_timer = lv_timer_create(GpsTimerCallback, kGpsTimerMs, state);
  if (state->tile_timer == nullptr || state->gps_timer == nullptr) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Create map timers failed\n");
  }
  return container;
}

}  // namespace lilygo_box::ui
