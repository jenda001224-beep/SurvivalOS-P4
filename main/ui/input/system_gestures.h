/*
 * @Description: iOS-style system gesture layer (home + control center)
 * @License: GPL 3.0
 */
#pragma once

#include <functional>

#include "lvgl.h"

namespace lilygo_box::ui {

// Callbacks driven by the two full-screen system gestures. All of them run on
// the LVGL task, so they may touch LVGL objects directly.
struct SystemGestureCallbacks {
  // Live progress of the swipe up from the bottom edge, 0..1000. Sent while the
  // finger is down so the shell can shrink the foreground app behind the drag.
  std::function<void(int progress_permille)> home_drag;
  // The bottom swipe was released. `committed` is true when it travelled far
  // enough (or fast enough) to actually go home.
  std::function<void(bool committed)> home_release;
  // Live progress of the swipe down from the top edge, 0..1000.
  std::function<void(int progress_permille)> control_center_drag;
  // The top swipe was released; `committed` is true when the panel should stay
  // open.
  std::function<void(bool committed)> control_center_release;
};

/**
 * @brief Installs the callbacks the gesture layer reports into.
 * @param callbacks Shell callbacks; any of them may be empty.
 */
void InitializeSystemGestures(SystemGestureCallbacks callbacks);

/**
 * @brief Feeds one already-rotated pointer sample into the gesture layer.
 * @param state LVGL pointer state for this sample.
 * @param point Pointer position, only meaningful while pressed.
 * @param screen_width Current display width in pixels.
 * @param screen_height Current display height in pixels.
 * @return true when the gesture layer has taken over this touch sequence and
 *         LVGL must not see the rest of it.
 */
bool HandleSystemGesturePointerInput(lv_indev_state_t state, lv_point_t point,
    int screen_width, int screen_height);

/**
 * @brief Enables or disables both system gestures.
 * @param enabled false while a view (a game, a full-screen viewer) wants the
 *        whole screen to itself.
 */
void SetSystemGesturesEnabled(bool enabled);

/**
 * @brief Reports whether the system gestures currently react to touch.
 * @return true when the gestures are enabled.
 */
bool AreSystemGesturesEnabled();

/**
 * @brief Shows or hides the home indicator bar along the bottom edge.
 * @param visible true to show the bar.
 */
void SetHomeIndicatorVisible(bool visible);

/**
 * @brief Sets the home indicator colour, so it stays readable on any content.
 * @param color Bar colour in 0xRRGGBB form.
 */
void SetHomeIndicatorColor(uint32_t color);

}  // namespace lilygo_box::ui
