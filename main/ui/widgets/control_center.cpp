/*
 * @Description: iOS-style control center overlay
 * @License: GPL 3.0
 */
#include "ui/widgets/control_center.h"

#include <algorithm>

#include "base/logger.h"
#include "ui/haptic_feedback.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"

namespace lilygo_box::ui {
namespace {

// The control center keeps its own dark palette instead of following the system
// theme, the way iOS does: it always reads as glass over whatever is behind it.
constexpr uint32_t kPanelColor = 0x1C1C1E;
constexpr uint32_t kModuleColor = 0x2C2C2E;
constexpr uint32_t kTrackColor = 0x3A3A3C;
constexpr uint32_t kAccentColor = 0x0A84FF;
constexpr uint32_t kOnDarkColor = 0xFFFFFF;
constexpr uint32_t kSliderFillColor = 0xF2F2F7;
constexpr uint32_t kSliderIconColor = 0x8E8E93;
constexpr uint32_t kBackdropColor = 0x000000;

// The panel is fully opaque and the backdrop never dims. A translucent panel or
// a backdrop whose opacity follows the drag makes LVGL re-blend the whole
// 568x1232 screen in software on every frame, which is what made the slide
// stutter; this way only the strip the panel moves across is repainted.
constexpr uint32_t kPanelBorderColor = 0x48484A;

constexpr int kPanelSideMargin = 22;
constexpr int kPanelTopMargin = 24;
constexpr int kPanelPadding = 20;
constexpr int kPanelRadius = 38;
constexpr int kModuleRadius = 30;

constexpr int kToggleCount = 4;
constexpr int kToggleMinimumSize = 76;
constexpr int kToggleMaximumSize = 112;
constexpr int kToggleRowGap = 24;
constexpr int kSliderHeight = 300;
constexpr int kSliderGap = 20;
constexpr int kSliderIconBottomMargin = 22;

constexpr uint32_t kOpenAnimationMs = 260;
constexpr uint32_t kCloseAnimationMs = 220;
constexpr int kProgressFull = 1000;

/**
 * @brief Applies a size and colour to a label.
 * @param label Label to style.
 * @param font Font to use.
 * @param color Text colour in 0xRRGGBB form.
 */
void StyleLabel(lv_obj_t* label, const lv_font_t* font, uint32_t color) {
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
}

}  // namespace

bool ControlCenter::Init(const ControlCenterConfig& config) {
  Destroy();
  config_ = config;

  lv_obj_t* layer = lv_layer_top();
  if (layer == nullptr) {
    return false;
  }

  const int layer_width = lv_obj_get_width(layer);
  const int layer_height = lv_obj_get_height(layer);
  if (layer_width <= 0 || layer_height <= 0) {
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Control center has no layout yet, width=%d, height=%d\n", layer_width,
        layer_height);
    return false;
  }

  backdrop_ = lv_obj_create(layer);
  if (backdrop_ == nullptr) {
    return false;
  }
  lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_FLOATING);
  // The backdrop paints nothing; it only catches taps outside the panel. It is
  // switched on and off through CLICKABLE rather than HIDDEN, because hiding or
  // showing a full-screen object invalidates the whole display and that one
  // full repaint is a visible hitch at the start of every swipe.
  lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(backdrop_, layer_width, layer_height);
  lv_obj_set_pos(backdrop_, 0, 0);
  lv_obj_set_style_radius(backdrop_, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(backdrop_, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(backdrop_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(backdrop_, lv_color_hex(kBackdropColor),
      LV_PART_MAIN);
  lv_obj_set_style_bg_opa(backdrop_, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_add_event_cb(
      backdrop_, BackdropClickedCallback, LV_EVENT_CLICKED, this);

  const int panel_width = layer_width - 2 * kPanelSideMargin;
  int toggle_size = (panel_width - 2 * kPanelPadding -
                        (kToggleCount - 1) * (kPanelPadding / 2)) /
      kToggleCount;
  toggle_size =
      std::clamp(toggle_size, kToggleMinimumSize, kToggleMaximumSize);
  panel_top_margin_ = kPanelTopMargin;
  panel_height_ = kPanelPadding + toggle_size + kToggleRowGap + kSliderHeight +
      kPanelPadding;

  panel_ = lv_obj_create(layer);
  if (panel_ == nullptr) {
    Destroy();
    return false;
  }
  lv_obj_remove_flag(panel_, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(panel_, LV_OBJ_FLAG_IGNORE_LAYOUT);
  lv_obj_add_flag(panel_, LV_OBJ_FLAG_FLOATING);
  lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_size(panel_, panel_width, panel_height_);
  lv_obj_set_pos(panel_, kPanelSideMargin, -panel_height_);
  lv_obj_set_style_radius(panel_, kPanelRadius, LV_PART_MAIN);
  // A hairline border stands in for the dimmed backdrop in setting the panel
  // apart from whatever is underneath.
  lv_obj_set_style_border_width(panel_, 1, LV_PART_MAIN);
  lv_obj_set_style_border_color(
      panel_, lv_color_hex(kPanelBorderColor), LV_PART_MAIN);
  lv_obj_set_style_pad_all(panel_, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(panel_, lv_color_hex(kPanelColor), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, LV_PART_MAIN);

  const int toggle_gap =
      (panel_width - 2 * kPanelPadding - kToggleCount * toggle_size) /
      std::max(1, kToggleCount - 1);
  int toggle_x = kPanelPadding;
  const int toggle_y = kPanelPadding;
  const bool toggles_created =
      CreateToggle(panel_, &wifi_toggle_, icon::kSignalWifi4Bar, toggle_x,
          toggle_y, toggle_size) &&
      CreateToggle(panel_, &bluetooth_toggle_, icon::kBluetooth,
          toggle_x += toggle_size + toggle_gap, toggle_y, toggle_size) &&
      CreateToggle(panel_, &silent_toggle_, icon::kVolumeOff,
          toggle_x += toggle_size + toggle_gap, toggle_y, toggle_size) &&
      CreateToggle(panel_, &lock_toggle_, icon::kLock,
          toggle_x += toggle_size + toggle_gap, toggle_y, toggle_size);
  if (!toggles_created) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Create control center toggles failed\n");
    Destroy();
    return false;
  }

  const int slider_y = kPanelPadding + toggle_size + kToggleRowGap;
  const int slider_width =
      (panel_width - 2 * kPanelPadding - kSliderGap) / 2;
  brightness_slider_ = CreateSlider(panel_, icon::kSunny, kPanelPadding,
      slider_y, slider_width, kSliderHeight);
  volume_slider_ = CreateSlider(panel_, icon::kVolumeUp,
      kPanelPadding + slider_width + kSliderGap, slider_y, slider_width,
      kSliderHeight);
  if (brightness_slider_ == nullptr || volume_slider_ == nullptr) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__,
        "Create control center sliders failed\n");
    Destroy();
    return false;
  }

  Refresh();
  return true;
}

void ControlCenter::Destroy() {
  if (panel_ != nullptr) {
    lv_anim_delete(this, SetPanelOffset);
    lv_obj_delete(panel_);
  }
  if (backdrop_ != nullptr) {
    lv_obj_delete(backdrop_);
  }
  panel_ = nullptr;
  backdrop_ = nullptr;
  brightness_slider_ = nullptr;
  volume_slider_ = nullptr;
  wifi_toggle_ = {};
  bluetooth_toggle_ = {};
  silent_toggle_ = {};
  lock_toggle_ = {};
  panel_height_ = 0;
  open_ = false;
}

bool ControlCenter::Relayout() {
  const ControlCenterConfig config = config_;
  return Init(config);
}

bool ControlCenter::IsVisible() const {
  return panel_ != nullptr && !lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN);
}

int ControlCenter::PanelTravel() const {
  return panel_height_ + panel_top_margin_;
}

void ControlCenter::SetPanelOffset(void* target, int32_t value) {
  auto* self = static_cast<ControlCenter*>(target);
  if (self == nullptr || self->panel_ == nullptr) {
    return;
  }

  const int travel = self->PanelTravel();
  const int offset = std::clamp(static_cast<int>(value), 0, travel);
  lv_obj_set_y(self->panel_, self->panel_top_margin_ - offset);
}

void ControlCenter::SetDragProgress(int progress_permille) {
  if (panel_ == nullptr) {
    return;
  }

  const int progress = std::clamp(progress_permille, 0, kProgressFull);
  if (progress == 0 && !open_) {
    lv_anim_delete(this, SetPanelOffset);
    SetPanelOffset(this, PanelTravel());
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    if (backdrop_ != nullptr) {
      lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);
    }
    return;
  }

  lv_anim_delete(this, SetPanelOffset);
  if (lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN)) {
    Refresh();
    lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    if (backdrop_ != nullptr) {
      lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_move_to_index(panel_, -1);
  }
  const int travel = PanelTravel();
  SetPanelOffset(this, travel - travel * progress / kProgressFull);
}

void ControlCenter::Open() {
  if (panel_ == nullptr) {
    return;
  }

  Refresh();
  lv_anim_delete(this, SetPanelOffset);
  if (lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN)) {
    SetPanelOffset(this, PanelTravel());
    lv_obj_remove_flag(panel_, LV_OBJ_FLAG_HIDDEN);
  }
  if (backdrop_ != nullptr) {
    lv_obj_add_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);
  }
  lv_obj_move_to_index(panel_, -1);
  open_ = true;

  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, this);
  lv_anim_set_values(
      &animation, panel_top_margin_ - lv_obj_get_y(panel_), 0);
  lv_anim_set_duration(&animation, kOpenAnimationMs);
  lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&animation, SetPanelOffset);
  if (lv_anim_start(&animation) == nullptr) {
    SetPanelOffset(this, 0);
  }
}

void ControlCenter::CloseCompletedCallback(lv_anim_t* animation) {
  auto* self = static_cast<ControlCenter*>(lv_anim_get_user_data(animation));
  if (self == nullptr || self->panel_ == nullptr) {
    return;
  }
  lv_obj_add_flag(self->panel_, LV_OBJ_FLAG_HIDDEN);
  if (self->backdrop_ != nullptr) {
    lv_obj_remove_flag(self->backdrop_, LV_OBJ_FLAG_CLICKABLE);
  }
}

void ControlCenter::Close() {
  if (panel_ == nullptr) {
    return;
  }

  open_ = false;
  lv_anim_delete(this, SetPanelOffset);
  if (lv_obj_has_flag(panel_, LV_OBJ_FLAG_HIDDEN)) {
    return;
  }

  lv_anim_t animation;
  lv_anim_init(&animation);
  lv_anim_set_var(&animation, this);
  lv_anim_set_user_data(&animation, this);
  lv_anim_set_values(
      &animation, panel_top_margin_ - lv_obj_get_y(panel_), PanelTravel());
  lv_anim_set_duration(&animation, kCloseAnimationMs);
  lv_anim_set_path_cb(&animation, lv_anim_path_ease_in);
  lv_anim_set_exec_cb(&animation, SetPanelOffset);
  lv_anim_set_completed_cb(&animation, CloseCompletedCallback);
  if (lv_anim_start(&animation) == nullptr) {
    SetPanelOffset(this, PanelTravel());
    lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
    if (backdrop_ != nullptr) {
      lv_obj_remove_flag(backdrop_, LV_OBJ_FLAG_CLICKABLE);
    }
  }
}

void ControlCenter::Refresh() {
  if (panel_ == nullptr) {
    return;
  }

  if (brightness_slider_ != nullptr && config_.get_brightness_percent) {
    lv_slider_set_value(brightness_slider_,
        std::clamp(config_.get_brightness_percent(), 0, 100), LV_ANIM_OFF);
  }
  if (volume_slider_ != nullptr && config_.get_volume_percent) {
    lv_slider_set_value(volume_slider_,
        std::clamp(config_.get_volume_percent(), 0, 100), LV_ANIM_OFF);
  }
  if (config_.get_wifi_enabled) {
    ApplyToggleState(&wifi_toggle_, config_.get_wifi_enabled());
  }
  if (config_.get_bluetooth_enabled) {
    ApplyToggleState(&bluetooth_toggle_, config_.get_bluetooth_enabled());
  }
  if (config_.get_silent_mode) {
    ApplyToggleState(&silent_toggle_, config_.get_silent_mode());
  }
  ApplyToggleState(&lock_toggle_, false);
}

void ControlCenter::ApplyToggleState(ToggleButton* toggle, bool active) {
  if (toggle == nullptr || toggle->button == nullptr) {
    return;
  }
  toggle->active = active;
  lv_obj_set_style_bg_color(toggle->button,
      lv_color_hex(active ? kAccentColor : kTrackColor), LV_PART_MAIN);
  if (toggle->icon != nullptr) {
    lv_obj_set_style_text_color(
        toggle->icon, lv_color_hex(kOnDarkColor), LV_PART_MAIN);
  }
}

bool ControlCenter::CreateToggle(lv_obj_t* parent, ToggleButton* toggle,
    const char* symbol, int x, int y, int size) {
  if (parent == nullptr || toggle == nullptr) {
    return false;
  }

  toggle->button = lv_obj_create(parent);
  if (toggle->button == nullptr) {
    return false;
  }
  lv_obj_remove_flag(toggle->button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(toggle->button, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(toggle->button, size, size);
  lv_obj_set_pos(toggle->button, x, y);
  lv_obj_set_style_radius(toggle->button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(toggle->button, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(toggle->button, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(
      toggle->button, lv_color_hex(kTrackColor), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(toggle->button, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_event_cb(
      toggle->button, ToggleClickedCallback, LV_EVENT_CLICKED, this);

  toggle->icon = lv_label_create(toggle->button);
  if (toggle->icon == nullptr) {
    return false;
  }
  lv_obj_remove_flag(toggle->icon, LV_OBJ_FLAG_CLICKABLE);
  lv_label_set_text(toggle->icon, symbol);
  StyleLabel(toggle->icon, &lvgl_font_material_symbols_fill_32, kOnDarkColor);
  lv_obj_center(toggle->icon);
  return true;
}

lv_obj_t* ControlCenter::CreateSlider(lv_obj_t* parent, const char* symbol,
    int x, int y, int width, int height) {
  if (parent == nullptr) {
    return nullptr;
  }

  lv_obj_t* slider = lv_slider_create(parent);
  if (slider == nullptr) {
    return nullptr;
  }
  lv_obj_set_size(slider, width, height);
  lv_obj_set_pos(slider, x, y);
  lv_slider_set_range(slider, 0, 100);
  lv_obj_set_style_radius(slider, kModuleRadius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(slider, lv_color_hex(kModuleColor), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(slider, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(slider, kModuleRadius, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(
      slider, lv_color_hex(kSliderFillColor), LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_INDICATOR);
  // iOS sliders have no visible knob; the fill level is the whole affordance.
  lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
  lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
  lv_obj_add_event_cb(slider, SliderChangedCallback, LV_EVENT_VALUE_CHANGED,
      this);

  lv_obj_t* icon = lv_label_create(slider);
  if (icon != nullptr) {
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(icon, symbol);
    StyleLabel(icon, &lvgl_font_material_symbols_fill_32, kSliderIconColor);
    lv_obj_align(icon, LV_ALIGN_BOTTOM_MID, 0, -kSliderIconBottomMargin);
  }
  return slider;
}

void ControlCenter::ToggleClickedCallback(lv_event_t* event) {
  auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(event));
  lv_obj_t* target = lv_event_get_target_obj(event);
  if (self == nullptr || target == nullptr) {
    return;
  }

  if (target == self->lock_toggle_.button) {
    PlayUiHapticEffect(HapticEffect::kImpactMedium);
    self->Close();
    if (self->config_.request_screen_lock) {
      self->config_.request_screen_lock();
    }
    return;
  }

  ToggleButton* toggle = nullptr;
  std::function<void(bool)> setter;
  if (target == self->wifi_toggle_.button) {
    toggle = &self->wifi_toggle_;
    setter = self->config_.set_wifi_enabled;
  } else if (target == self->bluetooth_toggle_.button) {
    toggle = &self->bluetooth_toggle_;
    setter = self->config_.set_bluetooth_enabled;
  } else if (target == self->silent_toggle_.button) {
    toggle = &self->silent_toggle_;
    setter = self->config_.set_silent_mode;
  }
  if (toggle == nullptr) {
    return;
  }

  const bool next_state = !toggle->active;
  self->ApplyToggleState(toggle, next_state);
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  if (setter) {
    setter(next_state);
  }
}

void ControlCenter::SliderChangedCallback(lv_event_t* event) {
  auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(event));
  lv_obj_t* target = lv_event_get_target_obj(event);
  if (self == nullptr || target == nullptr) {
    return;
  }

  const int value = std::clamp(
      static_cast<int>(lv_slider_get_value(target)), 0, 100);
  if (target == self->brightness_slider_ &&
      self->config_.set_brightness_percent) {
    self->config_.set_brightness_percent(value);
  } else if (target == self->volume_slider_ &&
      self->config_.set_volume_percent) {
    self->config_.set_volume_percent(value);
  }
}

void ControlCenter::BackdropClickedCallback(lv_event_t* event) {
  auto* self = static_cast<ControlCenter*>(lv_event_get_user_data(event));
  if (self != nullptr) {
    PlayUiHapticEffect(HapticEffect::kSelection);
    self->Close();
  }
}

}  // namespace lilygo_box::ui
