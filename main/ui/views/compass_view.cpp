/*
 * @Description: Compass app view
 * @License: GPL 3.0
 */
#include "ui/views/compass_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>

#include "app/storage/compass_storage.h"
#include "base/logger.h"
#include "hal/providers/imu_provider.h"
#include "ui/haptic_feedback.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"

namespace lilygo_box::ui {
namespace {

constexpr uint32_t kSampleIntervalMs = 60;
// Exponential smoothing on the heading. Raw magnetometer headings jitter by a
// couple of degrees; this settles the needle without making it feel laggy.
constexpr float kHeadingSmoothing = 0.22F;
// Below this the device is tilted too far for a tilt-compensated heading to be
// trustworthy, so the view says so instead of lying.
constexpr float kLevelWarningDegrees = 35.0F;

// A calibration pass needs the field to be swung through a decent range on
// every axis, otherwise the min/max centre is meaningless.
constexpr float kCalibrationMinimumSpanUt = 25.0F;
constexpr uint32_t kCalibrationMinimumMs = 6000;
constexpr uint32_t kCalibrationTimeoutMs = 45000;

constexpr int kDialTickCount = 72;
constexpr int kDialMajorTickEvery = 9;
constexpr int kDialTickWidth = 3;
constexpr int kDialTickLength = 16;
constexpr int kDialMajorTickWidth = 5;
constexpr int kDialMajorTickLength = 28;
constexpr int kCardinalInset = 60;
constexpr int kNorthMarkerSize = 18;

constexpr float kPi = 3.14159265F;

constexpr uint32_t kNorthColor = 0xE5484D;
constexpr uint32_t kDialColor = 0x8A8A8E;
constexpr uint32_t kMajorTickColor = 0xE6E6E6;

struct CardinalPoint {
  const char* label;
  int degrees;
};

constexpr CardinalPoint kCardinalPoints[] = {
    {.label = "N", .degrees = 0},
    {.label = "E", .degrees = 90},
    {.label = "S", .degrees = 180},
    {.label = "W", .degrees = 270},
};

// State that lives for as long as the view does. It is owned by the view root
// and freed from its LV_EVENT_DELETE handler.
struct CompassViewState {
  hal::ImuProvider* imu = nullptr;
  lv_obj_t* root = nullptr;
  lv_obj_t* dial = nullptr;
  lv_obj_t* heading_label = nullptr;
  lv_obj_t* cardinal_label = nullptr;
  lv_obj_t* status_label = nullptr;
  lv_obj_t* tilt_label = nullptr;
  lv_obj_t* calibrate_button = nullptr;
  lv_obj_t* calibrate_label = nullptr;
  lv_timer_t* timer = nullptr;

  bool heading_valid = false;
  float smoothed_heading_deg = 0.0F;
  int last_rendered_decidegrees = -1;
  int last_announced_cardinal = -1;

  bool calibrating = false;
  uint32_t calibration_start_tick = 0;
  float minimum_ut[3] = {0.0F, 0.0F, 0.0F};
  float maximum_ut[3] = {0.0F, 0.0F, 0.0F};
  bool calibration_seeded = false;
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
 * @brief Names the 45-degree sector a heading falls in.
 * @param heading_deg Heading in degrees.
 * @return Cardinal or intercardinal abbreviation.
 */
const char* CardinalName(float heading_deg) {
  static const char* kNames[] = {
      "N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  const int sector =
      static_cast<int>(std::lround(heading_deg / 45.0F)) & 0x7;
  return kNames[sector];
}

/**
 * @brief Smooths a heading, taking the short way around the 360-degree wrap.
 * @param previous_deg Previously smoothed heading.
 * @param sample_deg New raw heading.
 * @return The smoothed heading in 0..360.
 */
float SmoothHeading(float previous_deg, float sample_deg) {
  float delta = sample_deg - previous_deg;
  while (delta > 180.0F) {
    delta -= 360.0F;
  }
  while (delta < -180.0F) {
    delta += 360.0F;
  }
  float result = previous_deg + delta * kHeadingSmoothing;
  while (result < 0.0F) {
    result += 360.0F;
  }
  while (result >= 360.0F) {
    result -= 360.0F;
  }
  return result;
}

/**
 * @brief Places one tick mark around the dial.
 * @param dial Dial container.
 * @param index Tick index.
 * @param radius Dial radius in pixels.
 * @return true when the tick was created.
 */
bool CreateDialTick(lv_obj_t* dial, int index, int radius) {
  const bool major = index % kDialMajorTickEvery == 0;
  lv_obj_t* tick = lv_obj_create(dial);
  if (tick == nullptr) {
    return false;
  }
  lv_obj_remove_flag(tick, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(tick, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_border_width(tick, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(tick, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(tick, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(tick, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(tick,
      lv_color_hex(index == 0 ? kNorthColor
                              : (major ? kMajorTickColor : kDialColor)),
      LV_PART_MAIN);

  const int width = major ? kDialMajorTickWidth : kDialTickWidth;
  const int length = major ? kDialMajorTickLength : kDialTickLength;
  lv_obj_set_size(tick, width, length);

  // Ticks are laid out around the circle and then spun to face outwards.
  const float angle_rad =
      static_cast<float>(index) * 2.0F * kPi /
      static_cast<float>(kDialTickCount);
  const int centre = radius;
  const int distance = radius - length / 2 - 6;
  const int x = centre +
      static_cast<int>(std::lround(std::sin(angle_rad) * distance)) -
      width / 2;
  const int y = centre -
      static_cast<int>(std::lround(std::cos(angle_rad) * distance)) -
      length / 2;
  lv_obj_set_pos(tick, x, y);
  lv_obj_set_style_transform_pivot_x(tick, width / 2, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(tick, length / 2, LV_PART_MAIN);
  lv_obj_set_style_transform_rotation(
      tick, index * 3600 / kDialTickCount, LV_PART_MAIN);
  return true;
}

/**
 * @brief Builds the rotating dial: ticks, cardinal letters and the north wedge.
 * @param parent Parent object.
 * @param diameter Dial diameter in pixels.
 * @return The dial container, or nullptr on failure.
 */
lv_obj_t* CreateDial(lv_obj_t* parent, int diameter) {
  lv_obj_t* dial = lv_obj_create(parent);
  if (dial == nullptr) {
    return nullptr;
  }
  lv_obj_remove_flag(dial, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(dial, LV_OBJ_FLAG_CLICKABLE);
  MakeTransparent(dial);
  lv_obj_set_size(dial, diameter, diameter);
  lv_obj_set_style_transform_pivot_x(dial, diameter / 2, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(dial, diameter / 2, LV_PART_MAIN);

  const int radius = diameter / 2;
  for (int index = 0; index < kDialTickCount; ++index) {
    if (!CreateDialTick(dial, index, radius)) {
      lv_obj_delete(dial);
      return nullptr;
    }
  }

  for (const CardinalPoint& point : kCardinalPoints) {
    lv_obj_t* label = CreateLabel(dial, point.label,
        point.degrees == 0 ? kNorthColor : kMajorTickColor,
        &lvgl_font_google_sans_flex_36);
    if (label == nullptr) {
      lv_obj_delete(dial);
      return nullptr;
    }
    const float angle_rad = static_cast<float>(point.degrees) *
        kPi / 180.0F;
    const int distance = radius - kCardinalInset;
    lv_obj_update_layout(label);
    const int label_width = lv_obj_get_width(label);
    const int label_height = lv_obj_get_height(label);
    lv_obj_set_pos(label,
        radius +
            static_cast<int>(std::lround(std::sin(angle_rad) * distance)) -
            label_width / 2,
        radius -
            static_cast<int>(std::lround(std::cos(angle_rad) * distance)) -
            label_height / 2);
  }
  return dial;
}

/**
 * @brief Restarts a calibration pass from a clean slate.
 * @param state View state.
 */
void BeginCalibration(CompassViewState* state) {
  state->calibrating = true;
  state->calibration_seeded = false;
  state->calibration_start_tick = lv_tick_get();
  if (state->calibrate_label != nullptr) {
    lv_label_set_text(state->calibrate_label, "Cancel");
  }
  if (state->status_label != nullptr) {
    lv_label_set_text(state->status_label,
        "Wave the device in a figure eight");
  }
}

/**
 * @brief Leaves calibration mode and restores the idle labels.
 * @param state View state.
 * @param message Status text to show, or nullptr to clear it.
 */
void EndCalibration(CompassViewState* state, const char* message) {
  state->calibrating = false;
  if (state->calibrate_label != nullptr) {
    lv_label_set_text(state->calibrate_label, "Calibrate");
  }
  if (state->status_label != nullptr) {
    lv_label_set_text(state->status_label, message == nullptr ? "" : message);
  }
}

/**
 * @brief Feeds one raw sample into the running calibration.
 * @param state View state.
 * @param status Latest IMU sample.
 */
void UpdateCalibration(
    CompassViewState* state, const hal::ImuStatus& status) {
  const float sample[3] = {
      status.magnetic_x_ut, status.magnetic_y_ut, status.magnetic_z_ut};
  if (!state->calibration_seeded) {
    for (int axis = 0; axis < 3; ++axis) {
      state->minimum_ut[axis] = sample[axis];
      state->maximum_ut[axis] = sample[axis];
    }
    state->calibration_seeded = true;
    return;
  }

  float smallest_span = 1e9F;
  for (int axis = 0; axis < 3; ++axis) {
    state->minimum_ut[axis] = std::min(state->minimum_ut[axis], sample[axis]);
    state->maximum_ut[axis] = std::max(state->maximum_ut[axis], sample[axis]);
    smallest_span = std::min(
        smallest_span, state->maximum_ut[axis] - state->minimum_ut[axis]);
  }

  const uint32_t elapsed_ms = lv_tick_elaps(state->calibration_start_tick);
  if (state->status_label != nullptr) {
    char text[64];
    const int progress = std::clamp(
        static_cast<int>(smallest_span * 100.0F / kCalibrationMinimumSpanUt),
        0, 100);
    std::snprintf(text, sizeof(text), "Calibrating %d%%", progress);
    lv_label_set_text(state->status_label, text);
  }

  if (smallest_span >= kCalibrationMinimumSpanUt &&
      elapsed_ms >= kCalibrationMinimumMs) {
    app::CompassCalibration calibration;
    calibration.calibrated = true;
    calibration.offset_x_ut =
        (state->minimum_ut[0] + state->maximum_ut[0]) / 2.0F;
    calibration.offset_y_ut =
        (state->minimum_ut[1] + state->maximum_ut[1]) / 2.0F;
    calibration.offset_z_ut =
        (state->minimum_ut[2] + state->maximum_ut[2]) / 2.0F;
    app::UpdateCompassCalibration(calibration);
    if (state->imu != nullptr) {
      state->imu->SetMagnetometerCalibration(calibration.offset_x_ut,
          calibration.offset_y_ut, calibration.offset_z_ut);
    }
    PlayUiHapticEffect(HapticEffect::kSuccess);
    EndCalibration(state, "Calibrated");
    return;
  }

  if (elapsed_ms >= kCalibrationTimeoutMs) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    EndCalibration(state, "Calibration timed out, try again");
  }
}

/**
 * @brief Rotates the dial and refreshes the readouts for one sample.
 * @param state View state.
 * @param status Latest IMU sample.
 */
void RenderHeading(CompassViewState* state, const hal::ImuStatus& status) {
  if (!state->heading_valid) {
    state->smoothed_heading_deg = status.yaw_deg;
    state->heading_valid = true;
  } else {
    state->smoothed_heading_deg =
        SmoothHeading(state->smoothed_heading_deg, status.yaw_deg);
  }

  // The dial spins opposite the heading so that north on the dial keeps
  // pointing at magnetic north while the fixed marker shows where the device
  // is aimed.
  const int decidegrees =
      static_cast<int>(std::lround(state->smoothed_heading_deg * 10.0F)) % 3600;
  if (decidegrees != state->last_rendered_decidegrees &&
      state->dial != nullptr) {
    lv_obj_set_style_transform_rotation(
        state->dial, (3600 - decidegrees) % 3600, LV_PART_MAIN);
    state->last_rendered_decidegrees = decidegrees;
  }

  if (state->heading_label != nullptr) {
    char text[16];
    std::snprintf(text, sizeof(text), "%d",
        static_cast<int>(std::lround(state->smoothed_heading_deg)) % 360);
    lv_label_set_text(state->heading_label, text);
  }
  if (state->cardinal_label != nullptr) {
    lv_label_set_text(
        state->cardinal_label, CardinalName(state->smoothed_heading_deg));
  }

  // A short tick every time the needle crosses into a new 45-degree sector
  // gives the compass a physical feel without buzzing constantly.
  const int sector =
      static_cast<int>(std::lround(state->smoothed_heading_deg / 45.0F)) & 0x7;
  if (sector != state->last_announced_cardinal) {
    if (state->last_announced_cardinal >= 0) {
      PlayUiHapticEffect(HapticEffect::kSelection);
    }
    state->last_announced_cardinal = sector;
  }

  if (state->tilt_label != nullptr) {
    char text[48];
    std::snprintf(text, sizeof(text), "Pitch %d°   Roll %d°",
        static_cast<int>(std::lround(status.pitch_deg)),
        static_cast<int>(std::lround(status.roll_deg)));
    lv_label_set_text(state->tilt_label, text);
  }

  if (!state->calibrating && state->status_label != nullptr) {
    const bool tilted = std::fabs(status.pitch_deg) > kLevelWarningDegrees ||
        std::fabs(status.roll_deg) > kLevelWarningDegrees;
    const bool calibrated = app::GetCompassCalibration().calibrated;
    if (tilted) {
      lv_label_set_text(state->status_label, "Hold the device level");
    } else if (!calibrated) {
      lv_label_set_text(
          state->status_label, "Not calibrated — accuracy is poor");
    } else {
      lv_label_set_text(state->status_label, "");
    }
  }
}

/**
 * @brief Polls the IMU and drives the view.
 * @param timer LVGL timer.
 */
void SampleTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<CompassViewState*>(lv_timer_get_user_data(timer));
  if (state == nullptr || state->imu == nullptr) {
    return;
  }

  hal::ImuStatus status;
  if (!state->imu->ReadImuStatus(&status) || !status.ready) {
    if (!state->calibrating && state->status_label != nullptr) {
      lv_label_set_text(state->status_label, "Waiting for the sensor…");
    }
    return;
  }

  if (state->calibrating) {
    UpdateCalibration(state, status);
  }
  RenderHeading(state, status);
}

/**
 * @brief Starts or cancels calibration when the button is tapped.
 * @param event LVGL event.
 */
void CalibrateClickedCallback(lv_event_t* event) {
  auto* state =
      static_cast<CompassViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  if (state->calibrating) {
    EndCalibration(state, "Calibration cancelled");
  } else {
    BeginCalibration(state);
  }
}

/**
 * @brief Tears down the timer and state when the view is deleted.
 * @param event LVGL event.
 */
void ViewDeletedCallback(lv_event_t* event) {
  auto* state =
      static_cast<CompassViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  if (state->timer != nullptr) {
    lv_timer_delete(state->timer);
    state->timer = nullptr;
  }
  if (state->imu != nullptr) {
    state->imu->SetImuEnabled(false);
  }
  delete state;
}

}  // namespace

lv_obj_t* CreateCompassView(lv_obj_t* parent, const app::AppEntry& app_entry,
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
  // The compass reads best on a dark face whatever the system theme is.
  lv_obj_set_style_bg_color(container, lv_color_hex(0x101013), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container, LV_OPA_COVER, LV_PART_MAIN);

  auto* state = new (std::nothrow) CompassViewState();
  if (state == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  state->root = container;
  state->imu = config.imu;
  lv_obj_add_event_cb(container, ViewDeletedCallback, LV_EVENT_DELETE, state);

  const int diameter = std::min(config.width - 60, config.height / 2);
  lv_obj_t* dial = CreateDial(container, diameter);
  if (dial == nullptr) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__, "Create dial failed\n");
    lv_obj_delete(container);
    return nullptr;
  }
  state->dial = dial;
  lv_obj_align(dial, LV_ALIGN_CENTER, 0, 0);

  // Fixed marker at the top of the dial: it shows where the device points.
  lv_obj_t* marker = lv_obj_create(container);
  if (marker != nullptr) {
    lv_obj_remove_flag(marker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(marker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_border_width(marker, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(marker, 0, LV_PART_MAIN);
    lv_obj_set_size(marker, kNorthMarkerSize, kNorthMarkerSize);
    lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(marker, lv_color_hex(kNorthColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(marker, LV_ALIGN_CENTER, 0, -diameter / 2 - 18);
  }

  state->heading_label =
      CreateLabel(container, "0", 0xFFFFFF, &lvgl_font_google_sans_flex_64);
  if (state->heading_label != nullptr) {
    lv_obj_align(state->heading_label, LV_ALIGN_CENTER, 0, -22);
  }
  state->cardinal_label =
      CreateLabel(container, "N", 0xB4B4B8, &lvgl_font_google_sans_flex_36);
  if (state->cardinal_label != nullptr) {
    lv_obj_align(state->cardinal_label, LV_ALIGN_CENTER, 0, 38);
  }
  state->tilt_label = CreateLabel(
      container, "Pitch 0°   Roll 0°", 0x8A8A8E,
      &lvgl_font_google_sans_flex_24);
  if (state->tilt_label != nullptr) {
    lv_obj_align(state->tilt_label, LV_ALIGN_CENTER, 0,
        diameter / 2 + 46);
  }
  state->status_label =
      CreateLabel(container, "", 0xE0B341, &lvgl_font_google_sans_flex_24);
  if (state->status_label != nullptr) {
    lv_obj_set_width(state->status_label, config.width - 80);
    lv_obj_set_style_text_align(
        state->status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(state->status_label, LV_ALIGN_CENTER, 0, diameter / 2 + 86);
  }

  state->calibrate_button = lv_button_create(container);
  if (state->calibrate_button != nullptr) {
    lv_obj_set_size(state->calibrate_button, 240, 76);
    lv_obj_align(state->calibrate_button, LV_ALIGN_BOTTOM_MID, 0, -64);
    lv_obj_set_style_radius(state->calibrate_button, 38, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        state->calibrate_button, lv_color_hex(0x2C2C2E), LV_PART_MAIN);
    lv_obj_add_event_cb(state->calibrate_button, CalibrateClickedCallback,
        LV_EVENT_CLICKED, state);
    state->calibrate_label = CreateLabel(state->calibrate_button, "Calibrate",
        0xFFFFFF, &lvgl_font_google_sans_flex_24);
    if (state->calibrate_label != nullptr) {
      lv_obj_center(state->calibrate_label);
    }
  }

  // Leaving the app is handled by the global home gesture, so the view only
  // has to make sure the status bar stays readable on its dark face.
  if (config.set_status_bar_text_color) {
    config.set_status_bar_text_color(0xFFFFFF);
  }

  if (state->imu != nullptr) {
    state->imu->SetImuEnabled(true);
    const app::CompassCalibration calibration = app::GetCompassCalibration();
    if (calibration.calibrated) {
      state->imu->SetMagnetometerCalibration(calibration.offset_x_ut,
          calibration.offset_y_ut, calibration.offset_z_ut);
    }
  } else if (state->status_label != nullptr) {
    lv_label_set_text(state->status_label, "No motion sensor on this device");
  }

  state->timer =
      lv_timer_create(SampleTimerCallback, kSampleIntervalMs, state);
  if (state->timer == nullptr) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Create compass sample timer failed\n");
  }
  return container;
}

}  // namespace lilygo_box::ui
