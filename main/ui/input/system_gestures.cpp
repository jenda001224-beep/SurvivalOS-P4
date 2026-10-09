/*
 * @Description: iOS-style system gesture layer (home + control center)
 * @License: GPL 3.0
 */
#include "ui/input/system_gestures.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include "ui/haptic_feedback.h"

namespace lilygo_box::ui {
namespace {

// Height of the touch band along the bottom edge that arms the home gesture.
// Wide enough to catch a thumb coming off the bezel, narrow enough to leave the
// dock icons alone.
constexpr int kBottomBandHeight = 46;
// Height of the band along the top edge that arms the control center.
constexpr int kTopBandHeight = 46;
// Travel needed before a candidate turns into a real gesture and steals the
// touch from LVGL.
constexpr int kActivationDistance = 14;
// Sideways travel that cancels a candidate: the finger is scrolling, not
// swiping in from the edge.
constexpr int kCancelSidewaysDistance = 42;
// Fractions of the screen height the finger must cover to commit each gesture.
constexpr int kHomeCommitDivisor = 7;
constexpr int kControlCenterCommitDivisor = 6;
// A flick shorter than the commit distance still counts when it is fast enough.
constexpr int kFlickMinimumDistance = 32;
constexpr int kFlickMaximumDurationMs = 260;
constexpr int kProgressFull = 1000;

// Geometry of the home indicator bar.
constexpr int kIndicatorWidthDivisor = 3;
constexpr int kIndicatorHeight = 6;
constexpr int kIndicatorBottomMargin = 10;
constexpr lv_opa_t kIndicatorOpacity = 150;
// While the home gesture is dragged the bar itself follows the finger: it
// rises, widens and brightens. Nothing else on screen moves, so each frame
// repaints a strip a few pixels tall instead of the whole display.
constexpr int kIndicatorMaximumLift = 46;
constexpr int kIndicatorGrowthPercent = 22;

enum class GesturePhase {
  // No touch, or a touch that started away from either edge.
  kIdle,
  // Touch started inside an edge band but has not moved far enough yet.
  kBottomCandidate,
  kTopCandidate,
  // Gesture owns the touch sequence.
  kHomeActive,
  kControlCenterActive,
};

struct GestureState {
  GesturePhase phase = GesturePhase::kIdle;
  lv_point_t start_point = {};
  lv_point_t last_point = {};
  uint32_t start_tick = 0;
  bool pointer_pressed = false;
  bool committed = false;
};

SystemGestureCallbacks g_callbacks;
GestureState g_state;
bool g_enabled = true;
lv_obj_t* g_home_indicator = nullptr;
bool g_home_indicator_requested = false;
uint32_t g_home_indicator_color = 0xFFFFFF;
// Progress of the home gesture in permille, 0 when no gesture is active.
int g_home_indicator_progress = 0;

/**
 * @brief Clamps a value into an inclusive range.
 * @param value Value to clamp.
 * @param minimum Lower bound.
 * @param maximum Upper bound.
 * @return The clamped value.
 */
int ClampInt(int value, int minimum, int maximum) {
  return std::clamp(value, minimum, maximum);
}

/**
 * @brief Distance the gesture must cover to commit going home.
 * @param screen_height Current display height.
 * @return Commit distance in pixels.
 */
int HomeCommitDistance(int screen_height) {
  return std::max(60, screen_height / kHomeCommitDivisor);
}

/**
 * @brief Distance the gesture must cover to leave the control center open.
 * @param screen_height Current display height.
 * @return Commit distance in pixels.
 */
int ControlCenterCommitDistance(int screen_height) {
  return std::max(60, screen_height / kControlCenterCommitDivisor);
}

/**
 * @brief Decides whether a short gesture was flicked hard enough to commit.
 * @param travel Distance travelled in the gesture direction, in pixels.
 * @return true when the gesture was a committing flick.
 */
bool IsCommittingFlick(int travel) {
  const uint32_t duration_ms = lv_tick_elaps(g_state.start_tick);
  return travel >= kFlickMinimumDistance &&
      duration_ms <= static_cast<uint32_t>(kFlickMaximumDurationMs);
}

/**
 * @brief Converts travel into the 0..1000 progress the shell animates against.
 * @param travel Distance travelled in the gesture direction, in pixels.
 * @param commit_distance Distance that maps to full progress.
 * @return Progress in permille.
 */
int TravelToProgress(int travel, int commit_distance) {
  if (commit_distance <= 0) {
    return 0;
  }
  return ClampInt(travel * kProgressFull / commit_distance, 0, kProgressFull);
}

/**
 * @brief Updates the commit state and ticks the haptics as it flips.
 * @param travel Distance travelled in the gesture direction, in pixels.
 * @param commit_distance Distance that commits the gesture.
 */
void UpdateCommitState(int travel, int commit_distance) {
  const bool committed = travel >= commit_distance;
  if (committed != g_state.committed) {
    g_state.committed = committed;
    if (committed) {
      PlayUiHapticEffect(HapticEffect::kGestureCommit);
    }
  }
}

/**
 * @brief Creates the home indicator bar if it does not exist yet.
 * @return true when the bar is available.
 */
bool EnsureHomeIndicator() {
  if (g_home_indicator != nullptr) {
    return true;
  }

  lv_obj_t* layer = lv_layer_top();
  if (layer == nullptr) {
    return false;
  }

  g_home_indicator = lv_obj_create(layer);
  if (g_home_indicator == nullptr) {
    return false;
  }
  lv_obj_remove_flag(g_home_indicator, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(g_home_indicator, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(g_home_indicator, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_flag(g_home_indicator, LV_OBJ_FLAG_FLOATING);
  lv_obj_add_flag(g_home_indicator, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_border_width(g_home_indicator, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(g_home_indicator, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(g_home_indicator, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(
      g_home_indicator, lv_color_hex(g_home_indicator_color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_home_indicator, kIndicatorOpacity, LV_PART_MAIN);
  return true;
}

/**
 * @brief Re-applies the home indicator geometry and visibility.
 */
void RefreshHomeIndicator() {
  if (!EnsureHomeIndicator()) {
    return;
  }

  lv_obj_t* layer = lv_layer_top();
  const int layer_width = layer == nullptr ? 0 : lv_obj_get_width(layer);
  const int layer_height = layer == nullptr ? 0 : lv_obj_get_height(layer);
  if (layer_width <= 0 || layer_height <= 0) {
    return;
  }

  const int progress = ClampInt(g_home_indicator_progress, 0, kProgressFull);
  const int rest_width = std::max(80, layer_width / kIndicatorWidthDivisor);
  const int width = rest_width +
      rest_width * kIndicatorGrowthPercent * progress / (100 * kProgressFull);
  const int lift = kIndicatorMaximumLift * progress / kProgressFull;
  const int opacity = static_cast<int>(kIndicatorOpacity) +
      (static_cast<int>(LV_OPA_COVER) - static_cast<int>(kIndicatorOpacity)) *
          progress / kProgressFull;
  lv_obj_set_size(g_home_indicator, width, kIndicatorHeight);
  lv_obj_set_pos(g_home_indicator, (layer_width - width) / 2,
      layer_height - kIndicatorHeight - kIndicatorBottomMargin - lift);
  lv_obj_set_style_bg_color(
      g_home_indicator, lv_color_hex(g_home_indicator_color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(
      g_home_indicator, static_cast<lv_opa_t>(opacity), LV_PART_MAIN);
  if (g_home_indicator_requested) {
    lv_obj_remove_flag(g_home_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_to_index(g_home_indicator, -1);
  } else {
    lv_obj_add_flag(g_home_indicator, LV_OBJ_FLAG_HIDDEN);
  }
}

/**
 * @brief Reports the final outcome of the active gesture and clears the state.
 */
void FinishActiveGesture() {
  const GesturePhase phase = g_state.phase;
  const bool home = phase == GesturePhase::kHomeActive;
  const int travel = home
      ? g_state.start_point.y - g_state.last_point.y
      : g_state.last_point.y - g_state.start_point.y;
  // The distance criterion is already tracked while dragging; a short but fast
  // flick commits as well, which is what makes the gesture feel like iOS.
  const bool commit = g_state.committed || IsCommittingFlick(travel);

  g_state = {};
  if (g_home_indicator_progress != 0) {
    g_home_indicator_progress = 0;
    RefreshHomeIndicator();
  }

  if (home) {
    if (g_callbacks.home_release) {
      g_callbacks.home_release(commit);
    }
    if (commit) {
      PlayUiHapticEffect(HapticEffect::kImpactHeavy);
    }
    return;
  }
  if (g_callbacks.control_center_release) {
    g_callbacks.control_center_release(commit);
  }
  if (commit) {
    PlayUiHapticEffect(HapticEffect::kImpactMedium);
  }
}

}  // namespace

void InitializeSystemGestures(SystemGestureCallbacks callbacks) {
  g_callbacks = std::move(callbacks);
  g_state = {};
}

void SetSystemGesturesEnabled(bool enabled) {
  if (g_enabled == enabled) {
    return;
  }
  g_enabled = enabled;
  g_state = {};
  if (g_home_indicator_progress != 0) {
    g_home_indicator_progress = 0;
    RefreshHomeIndicator();
  }
}

bool AreSystemGesturesEnabled() { return g_enabled; }

void SetHomeIndicatorVisible(bool visible) {
  g_home_indicator_requested = visible;
  RefreshHomeIndicator();
}

void SetHomeIndicatorColor(uint32_t color) {
  g_home_indicator_color = color;
  RefreshHomeIndicator();
}

bool HandleSystemGesturePointerInput(lv_indev_state_t state, lv_point_t point,
    int screen_width, int screen_height) {
  if (!g_enabled) {
    return false;
  }

  if (state != LV_INDEV_STATE_PRESSED) {
    const bool was_active = g_state.phase == GesturePhase::kHomeActive ||
        g_state.phase == GesturePhase::kControlCenterActive;
    if (was_active) {
      FinishActiveGesture();
    } else {
      g_state = {};
    }
    return was_active;
  }

  if (screen_width <= 0 || screen_height <= 0) {
    return false;
  }

  if (!g_state.pointer_pressed) {
    g_state = {};
    g_state.pointer_pressed = true;
    g_state.start_point = point;
    g_state.last_point = point;
    g_state.start_tick = lv_tick_get();
    if (point.y >= screen_height - kBottomBandHeight) {
      g_state.phase = GesturePhase::kBottomCandidate;
    } else if (point.y <= kTopBandHeight) {
      g_state.phase = GesturePhase::kTopCandidate;
    } else {
      g_state.phase = GesturePhase::kIdle;
    }
    // Never swallow the initial press: taps on the dock and on the status bar
    // have to keep working.
    return false;
  }

  g_state.last_point = point;
  const int vertical = point.y - g_state.start_point.y;
  const int sideways = std::abs(point.x - g_state.start_point.x);

  switch (g_state.phase) {
    case GesturePhase::kBottomCandidate: {
      if (sideways > kCancelSidewaysDistance || vertical > kActivationDistance) {
        g_state.phase = GesturePhase::kIdle;
        return false;
      }
      if (-vertical < kActivationDistance || sideways > -vertical) {
        return false;
      }
      g_state.phase = GesturePhase::kHomeActive;
      PlayUiHapticEffect(HapticEffect::kSelection);
      break;
    }
    case GesturePhase::kTopCandidate: {
      if (sideways > kCancelSidewaysDistance ||
          -vertical > kActivationDistance) {
        g_state.phase = GesturePhase::kIdle;
        return false;
      }
      if (vertical < kActivationDistance || sideways > vertical) {
        return false;
      }
      g_state.phase = GesturePhase::kControlCenterActive;
      PlayUiHapticEffect(HapticEffect::kSelection);
      break;
    }
    case GesturePhase::kHomeActive:
    case GesturePhase::kControlCenterActive:
      break;
    case GesturePhase::kIdle:
      return false;
  }

  if (g_state.phase == GesturePhase::kHomeActive) {
    const int travel = std::max(0, -vertical);
    const int commit_distance = HomeCommitDistance(screen_height);
    UpdateCommitState(travel, commit_distance);
    const int progress = TravelToProgress(travel, commit_distance);
    if (progress != g_home_indicator_progress) {
      g_home_indicator_progress = progress;
      RefreshHomeIndicator();
    }
    if (g_callbacks.home_drag) {
      g_callbacks.home_drag(progress);
    }
    return true;
  }

  const int travel = std::max(0, vertical);
  const int commit_distance = ControlCenterCommitDistance(screen_height);
  UpdateCommitState(travel, commit_distance);
  if (g_callbacks.control_center_drag) {
    g_callbacks.control_center_drag(TravelToProgress(travel, commit_distance));
  }
  return true;
}

}  // namespace lilygo_box::ui
