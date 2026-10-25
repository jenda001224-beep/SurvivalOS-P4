/*
 * @Description: 锁屏时钟布局、拖动回弹与解锁动画实现
 * @Author: LILYGO_L
 * @Date: 2026-05-10 13:27:05
 * @LastEditTime: 2026-09-02 17:54:35
 * @License: GPL 3.0
 */
#include "ui/widgets/label_fit.h"
#include "ui/views/lock_screen_view.h"

#include <algorithm>

#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"
#include "ui/theme/survival_palette.h"
#include "ui/wallpaper.h"
#include "ui/widgets/battery_overview_card.h"
#include "ui/views/music_view.h"
#include "ui/haptic_feedback.h"

namespace lilygo_box::ui {
namespace {

// SurvivalOS: thin clock and date above the battery card.
constexpr int kLockTimeTop = 96;
constexpr int kLockDateGap = 6;
constexpr int kLockCardTop = 330;
constexpr int kLockHorizontalPadding = 10;
constexpr int kUnlockHintBottom = 46;
constexpr uint32_t kLockResetAnimationMs = 180;
constexpr uint32_t kLockUnlockAnimationMs = 220;

/**
 * @brief 获取锁屏时间字体
 * @return 字体指针
 */
[[maybe_unused]] const lv_font_t* HomeTimeFont() { return &lvgl_font_lineseedkr_rg_120; }

/**
 * @brief 获取锁屏日期字体
 * @return 字体指针
 */
[[maybe_unused]] const lv_font_t* HomeDateFont() { return &lvgl_font_lineseedkr_th_60; }

/**
 * @brief 获取 56 号 Material Symbols 字体
 * @return 字体指针
 */
const lv_font_t* MaterialOutlineIconFont56() {
  return &lvgl_font_material_symbols_outline_56;
}

/**
 * @brief 设置文本对象的颜色和字体
 * @param object LVGL 对象
 * @param color 文本颜色
 * @param font 文本字体
 */
void SetTextStyle(lv_obj_t* object, lv_color_t color, const lv_font_t* font) {
  lv_obj_set_style_text_color(object, color, LV_PART_MAIN);
  lv_obj_set_style_text_font(object, font, LV_PART_MAIN);
}

/**
 * @brief 清除对象背景、边框和内边距
 * @param object LVGL 对象
 */
void MakeTransparent(lv_obj_t* object) {
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
}

/**
 * @brief 创建指定字体文本标签
 * @param parent 父对象
 * @param text 显示文本
 * @param color 文本颜色
 * @param font 文本字体
 * @return 创建成功返回对象指针，否则返回 nullptr
 */
lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, lv_color_t color,
    const lv_font_t* font) {
  lv_obj_t* label = lv_label_create(parent);
  if (label == nullptr) {
    return nullptr;
  }
  lv_label_set_text(label, text);
  SetTextStyle(label, color, font);
  return label;
}

/**
 * @brief 设置对象 Y 坐标动画回调
 * @param object LVGL 对象
 * @param value Y 坐标
 */
void SetObjectY(void* object, int32_t value) {
  lv_obj_set_y(static_cast<lv_obj_t*>(object), value);
}

// SurvivalOS: the lock screen shows the battery card from Settings > Power
// Saving & Battery where upstream draws the clock, date and weekday. There is
// only ever one lock screen, so its card is kept here with the screen it
// belongs to rather than being looked up through the object tree.
BatteryOverviewCard g_battery_card;
lv_obj_t* g_battery_card_owner = nullptr;
lv_obj_t* g_time_label = nullptr;
lv_obj_t* g_date_label = nullptr;

// SurvivalOS: the music player on the lock screen, shown while a track is
// loaded (playing or paused), like the Now Playing card of a phone.
struct NowPlayingCard {
  lv_obj_t* card = nullptr;
  lv_obj_t* cover = nullptr;
  lv_obj_t* title = nullptr;
  lv_obj_t* artist = nullptr;
  lv_obj_t* slider = nullptr;
  lv_obj_t* elapsed = nullptr;
  lv_obj_t* remaining = nullptr;
  lv_obj_t* play_icon = nullptr;
  lv_obj_t* buttons[3] = {};  // previous, play/pause, next
  lv_timer_t* timer = nullptr;
  uint32_t serial = UINT32_MAX;
  bool seeking = false;
};
NowPlayingCard g_now_playing;

/**
 * @brief Forgets the battery card when its lock screen is deleted.
 * @param event LVGL delete event of the lock screen.
 */
void LockScreenDeleteEventCallback(lv_event_t* event) {
  if (lv_event_get_target_obj(event) == g_battery_card_owner) {
    g_battery_card = {};
    g_battery_card_owner = nullptr;
    g_time_label = nullptr;
    g_date_label = nullptr;
    if (g_now_playing.timer != nullptr) {
      lv_timer_delete(g_now_playing.timer);
    }
    g_now_playing = NowPlayingCard();
  }
}

/**
 * @brief Creates the battery card in the place of the upstream clock group.
 * @param lock_screen Lock screen object.
 * @param options Lock screen view options.
 * @return The transparent group holding the card, or nullptr.
 */
lv_obj_t* CreateBatteryGroup(
    lv_obj_t* lock_screen, const LockScreenViewOptions& options) {
  constexpr int kGroupHeight = 282;
  constexpr int kCardSideInset = 14;
  lv_obj_t* group = lv_obj_create(lock_screen);
  if (group == nullptr) {
    return nullptr;
  }
  lv_obj_remove_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(group, LV_OBJ_FLAG_CLICKABLE);
  MakeTransparent(group);
  lv_obj_set_style_pad_all(group, 0, LV_PART_MAIN);
  const int group_width = options.screen_width - 2 * kLockHorizontalPadding;
  lv_obj_set_size(group, group_width, kGroupHeight);
  lv_obj_align(group, LV_ALIGN_TOP_LEFT, kLockHorizontalPadding,
      kLockCardTop - (kGroupHeight - kBatteryOverviewCardHeight) / 2);

  BatteryOverviewCard card;
  if (!CreateBatteryOverviewCard(
          group, group_width - 2 * kCardSideInset, &card)) {
    lv_obj_delete(group);
    return nullptr;
  }
  lv_obj_set_pos(card.card, kCardSideInset,
      (kGroupHeight - kBatteryOverviewCardHeight) / 2);
  g_battery_card = card;
  g_battery_card_owner = lock_screen;
  lv_obj_add_event_cb(
      lock_screen, LockScreenDeleteEventCallback, LV_EVENT_DELETE, nullptr);
  return group;
}


void FormatTime(lv_obj_t* label, uint32_t ms, bool minus) {
  const unsigned seconds = ms / 1000U;
  lv_label_set_text_fmt(label, "%s%u:%02u", minus ? "-" : "", seconds / 60U, seconds % 60U);
}

void UpdateNowPlaying(lv_timer_t* /*timer*/) {
  NowPlayingCard& np = g_now_playing;
  if (np.card == nullptr) {
    return;
  }
  MusicNowPlaying now;
  if (!GetMusicNowPlaying(&now)) {
    lv_obj_add_flag(np.card, LV_OBJ_FLAG_HIDDEN);
    np.serial = UINT32_MAX;
    return;
  }
  lv_obj_remove_flag(np.card, LV_OBJ_FLAG_HIDDEN);
  if (now.track_serial != np.serial) {
    np.serial = now.track_serial;
    lv_label_set_text(np.title, now.title);
    lv_label_set_text(np.artist, now.artist);
    SetMusicNowPlayingCover(np.cover);
  }
  lv_label_set_text(np.play_icon, now.playing ? icon::kPause : icon::kPlayArrow);
  if (!np.seeking && now.duration_ms > 0) {
    lv_slider_set_value(np.slider,
        static_cast<int32_t>(static_cast<uint64_t>(now.elapsed_ms) * 1000 / now.duration_ms),
        LV_ANIM_OFF);
  }
  FormatTime(np.elapsed, now.elapsed_ms, false);
  FormatTime(np.remaining, now.duration_ms > now.elapsed_ms ? now.duration_ms - now.elapsed_ms : 0, true);
}

void NowPlayingButton(lv_event_t* event) {
  const auto action = reinterpret_cast<intptr_t>(lv_event_get_user_data(event));
  if (action == 0) {
    MusicTogglePlayback();
  } else {
    MusicSkipTrack(static_cast<int>(action));
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  UpdateNowPlaying(nullptr);
}

void NowPlayingSeek(lv_event_t* event) {
  NowPlayingCard& np = g_now_playing;
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_PRESSED) {
    np.seeking = true;
  } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    np.seeking = false;
    MusicNowPlaying now;
    if (GetMusicNowPlaying(&now) && now.duration_ms > 0) {
      MusicSeekTo(static_cast<uint32_t>(static_cast<uint64_t>(lv_slider_get_value(np.slider)) *
          now.duration_ms / 1000));
    }
  }
}

lv_obj_t* ControlButton(lv_obj_t* parent, const char* symbol, intptr_t action, int size) {
  lv_obj_t* button = lv_button_create(parent);
  lv_obj_set_size(button, size, size);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(button, 40, LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(button, NowPlayingButton, LV_EVENT_CLICKED, reinterpret_cast<void*>(action));
  lv_obj_t* label = lv_label_create(button);
  lv_label_set_text(label, symbol);
  lv_obj_set_style_text_font(label, &lvgl_font_material_symbols_fill_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_style_transform_scale(label, action == 0 ? 400 : 330, LV_PART_MAIN);
  lv_obj_center(label);
  return button;
}

void CreateNowPlayingCard(lv_obj_t* lock_screen, const LockScreenViewOptions& options, int top) {
  NowPlayingCard& np = g_now_playing;
  const int width = options.screen_width - 2 * (kLockHorizontalPadding + 14);
  lv_obj_t* card = lv_obj_create(lock_screen);
  lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(card, width, 330);
  lv_obj_set_pos(card, kLockHorizontalPadding + 14, top);
  lv_obj_set_style_radius(card, 34, LV_PART_MAIN);
  lv_obj_set_style_bg_color(card, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, 150, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 22, LV_PART_MAIN);
  np.card = card;
  np.cover = lv_obj_create(card);
  lv_obj_remove_flag(np.cover, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(np.cover, 120, 120);
  lv_obj_set_pos(np.cover, 0, 0);
  lv_obj_set_style_radius(np.cover, 18, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(np.cover, true, LV_PART_MAIN);
  lv_obj_set_style_bg_color(np.cover, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
  lv_obj_set_style_border_width(np.cover, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(np.cover, 0, LV_PART_MAIN);
  lv_obj_t* note = lv_label_create(np.cover);
  lv_label_set_text(note, icon::kMusic);
  lv_obj_set_style_text_font(note, &lvgl_font_material_symbols_fill_32, LV_PART_MAIN);
  lv_obj_set_style_text_color(note, lv_color_hex(0xAEAEB2), LV_PART_MAIN);
  lv_obj_center(note);
  const int text_left = 140;
  const int text_width = width - 44 - text_left;
  np.title = CreateLabel(card, "", lv_color_hex(0xFFFFFF), &lvgl_font_google_sans_flex_28);
  lv_obj_set_width(np.title, text_width);
  lv_label_set_long_mode(np.title, LV_LABEL_LONG_SCROLL_CIRCULAR);
  EnsureLabelLineHeight(np.title);
  lv_obj_set_pos(np.title, text_left, 22);
  np.artist = CreateLabel(card, "", lv_color_hex(0xAEAEB2), &lvgl_font_google_sans_flex_24);
  lv_obj_set_width(np.artist, text_width);
  lv_label_set_long_mode(np.artist, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(np.artist, text_left, 62);
  np.slider = lv_slider_create(card);
  lv_slider_set_range(np.slider, 0, 1000);
  lv_obj_set_size(np.slider, width - 44, 10);
  lv_obj_set_pos(np.slider, 0, 150);
  lv_obj_set_style_bg_color(np.slider, lv_color_hex(0x48484A), LV_PART_MAIN);
  lv_obj_set_style_bg_color(np.slider, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(np.slider, lv_color_hex(0xFFFFFF), LV_PART_KNOB);
  lv_obj_set_style_pad_all(np.slider, 6, LV_PART_KNOB);
  lv_obj_set_ext_click_area(np.slider, 24);
  lv_obj_add_event_cb(np.slider, NowPlayingSeek, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(np.slider, NowPlayingSeek, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(np.slider, NowPlayingSeek, LV_EVENT_PRESS_LOST, nullptr);
  np.elapsed = CreateLabel(card, "0:00", lv_color_hex(0xAEAEB2), &lvgl_font_google_sans_flex_22);
  lv_obj_set_pos(np.elapsed, 0, 172);
  np.remaining = CreateLabel(card, "-0:00", lv_color_hex(0xAEAEB2), &lvgl_font_google_sans_flex_22);
  lv_obj_align(np.remaining, LV_ALIGN_TOP_RIGHT, 0, 172);
  lv_obj_t* previous = ControlButton(card, icon::kSkipPrevious, -1, 84);
  lv_obj_align(previous, LV_ALIGN_BOTTOM_MID, -150, 0);
  lv_obj_t* play = ControlButton(card, icon::kPlayArrow, 0, 96);
  lv_obj_align(play, LV_ALIGN_BOTTOM_MID, 0, 6);
  np.play_icon = lv_obj_get_child(play, 0);
  lv_obj_t* next = ControlButton(card, icon::kSkipNext, 1, 84);
  lv_obj_align(next, LV_ALIGN_BOTTOM_MID, 150, 0);
  np.buttons[0] = previous;
  np.buttons[1] = play;
  np.buttons[2] = next;
  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
  np.timer = lv_timer_create(UpdateNowPlaying, 400, nullptr);
  UpdateNowPlaying(nullptr);
}

// The player touch in progress: -1 none, 0..2 a button, 3 the slider.
int g_player_touch = -1;

bool InsideObject(lv_obj_t* object, int x, int y, int margin) {
  if (object == nullptr) {
    return false;
  }
  lv_area_t area;
  lv_obj_get_coords(object, &area);
  return x >= area.x1 - margin && x <= area.x2 + margin && y >= area.y1 - margin &&
         y <= area.y2 + margin;
}

void SeekSliderTo(int x) {
  NowPlayingCard& np = g_now_playing;
  lv_area_t area;
  lv_obj_get_coords(np.slider, &area);
  const int width = std::max<int>(1, static_cast<int>(area.x2 - area.x1));
  const int value = std::clamp(static_cast<int>((x - area.x1) * 1000 / width), 0, 1000);
  lv_slider_set_value(np.slider, value, LV_ANIM_OFF);
}

}  // namespace

bool HandleLockScreenPlayerTouch(
    lv_obj_t* lock_screen, LockScreenTouchPhase phase, int x, int y) {
  NowPlayingCard& np = g_now_playing;
  if (lock_screen == nullptr || lock_screen != g_battery_card_owner || np.card == nullptr ||
      lv_obj_has_flag(np.card, LV_OBJ_FLAG_HIDDEN)) {
    g_player_touch = -1;
    return false;
  }
  switch (phase) {
    case LockScreenTouchPhase::kPress:
      g_player_touch = -1;
      for (int i = 0; i < 3; ++i) {
        if (InsideObject(np.buttons[i], x, y, 10)) {
          g_player_touch = i;
          lv_obj_add_state(np.buttons[i], LV_STATE_PRESSED);
          return true;
        }
      }
      if (InsideObject(np.slider, x, y, 28)) {
        g_player_touch = 3;
        np.seeking = true;
        SeekSliderTo(x);
        return true;
      }
      return false;
    case LockScreenTouchPhase::kMove:
      if (g_player_touch == 3) {
        SeekSliderTo(x);
      } else if (g_player_touch >= 0 && !InsideObject(np.buttons[g_player_touch], x, y, 30)) {
        lv_obj_remove_state(np.buttons[g_player_touch], LV_STATE_PRESSED);
      }
      return g_player_touch >= 0;
    case LockScreenTouchPhase::kRelease: {
      const int touch = g_player_touch;
      g_player_touch = -1;
      if (touch == 3) {
        np.seeking = false;
        MusicNowPlaying now;
        if (GetMusicNowPlaying(&now) && now.duration_ms > 0) {
          MusicSeekTo(static_cast<uint32_t>(
              static_cast<uint64_t>(lv_slider_get_value(np.slider)) * now.duration_ms / 1000));
        }
        PlayUiHapticEffect(HapticEffect::kSelection);
        UpdateNowPlaying(nullptr);
        return true;
      }
      if (touch >= 0) {
        const bool still_on_button = lv_obj_has_state(np.buttons[touch], LV_STATE_PRESSED);
        lv_obj_remove_state(np.buttons[touch], LV_STATE_PRESSED);
        if (still_on_button) {
          if (touch == 1) {
            MusicTogglePlayback();
          } else {
            MusicSkipTrack(touch == 0 ? -1 : 1);
          }
          PlayUiHapticEffect(HapticEffect::kSelection);
          UpdateNowPlaying(nullptr);
        }
        return true;
      }
      return false;
    }
  }
  return false;
}

lv_obj_t* CreateLockScreenView(
    lv_obj_t* parent, const LockScreenViewOptions& options) {
  if (parent == nullptr) {
    return nullptr;
  }

  lv_obj_t* lock_screen = lv_obj_create(parent);
  if (lock_screen == nullptr) {
    return nullptr;
  }
  lv_obj_remove_flag(lock_screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(lock_screen, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
  lv_obj_set_size(lock_screen, options.screen_width, options.screen_height);
  lv_obj_align(lock_screen, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_border_width(lock_screen, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(lock_screen, 0, LV_PART_MAIN);

  WallpaperObjects wallpaper;
  CreateWallpaperObjects(
      lock_screen, options.screen_width, options.screen_height, &wallpaper);
  // SurvivalOS: Survival Mode keeps the panel almost dark: no wallpaper.
  if (survival_look::Active()) {
    lv_obj_set_style_bg_color(lock_screen, lv_color_hex(survival_look::kBlack), LV_PART_MAIN);
    for (lv_obj_t* layer : wallpaper.layers) {
      if (layer != nullptr) {
        lv_obj_add_flag(layer, LV_OBJ_FLAG_HIDDEN);
      }
    }
  }

  if (CreateBatteryGroup(lock_screen, options) == nullptr) {
    lv_obj_delete(lock_screen);
    return nullptr;
  }

  // Thin digits: on an AMOLED only lit pixels cost power.
  const bool survival = survival_look::Active();
  const lv_color_t text_color = lv_color_hex(
      survival ? survival_look::kText : theme::FixedColors().lock_screen_text);
  g_time_label = CreateLabel(
      lock_screen, "--:--", text_color, &lvgl_font_lineseed_th_150);
  if (g_time_label != nullptr) {
    lv_obj_align(g_time_label, LV_ALIGN_TOP_MID, 0, kLockTimeTop);
  }
  g_date_label = CreateLabel(lock_screen, "",
      survival ? lv_color_hex(survival_look::kDim) : text_color,
      &lvgl_font_google_sans_flex_32);
  if (g_date_label != nullptr) {
    lv_obj_set_style_text_opa(g_date_label, survival ? LV_OPA_COVER : 210, LV_PART_MAIN);
    if (g_time_label != nullptr) {
      lv_obj_align_to(g_date_label, g_time_label, LV_ALIGN_OUT_BOTTOM_MID, 0,
          kLockDateGap);
    }
  }

  CreateNowPlayingCard(lock_screen, options, kLockCardTop + kBatteryOverviewCardHeight + 28);

  lv_obj_t* arrow = CreateLabel(lock_screen, icon::kKeyboardArrowUp,
      lv_color_hex(survival_look::Active() ? survival_look::kDim
                                           : theme::FixedColors().lock_screen_text),
      MaterialOutlineIconFont56());
  if (arrow == nullptr) {
    lv_obj_delete(lock_screen);
    return nullptr;
  }
  lv_obj_set_style_text_opa(arrow, 230, LV_PART_MAIN);
  lv_obj_align(arrow, LV_ALIGN_BOTTOM_MID, 0, -kUnlockHintBottom);

  lv_obj_move_to_index(lock_screen, -1);
  return lock_screen;
}

void UpdateLockScreenViewClock(lv_obj_t* lock_screen, const char* time_text,
    const char* date_text, const char* week_text) {
  if (lock_screen == nullptr || lock_screen != g_battery_card_owner) {
    return;
  }
  if (g_time_label != nullptr && time_text != nullptr && time_text[0] != '\0') {
    lv_label_set_text(g_time_label, time_text);
  }
  if (g_date_label != nullptr && date_text != nullptr) {
    // "Sat, Oct 24th"
    lv_label_set_text_fmt(g_date_label, "%s%s%s",
        week_text != nullptr ? week_text : "",
        week_text != nullptr && week_text[0] != '\0' ? ", " : "", date_text);
    if (g_time_label != nullptr) {
      lv_obj_align_to(g_date_label, g_time_label, LV_ALIGN_OUT_BOTTOM_MID, 0,
          kLockDateGap);
    }
  }
}

void UpdateLockScreenViewBattery(
    lv_obj_t* lock_screen, const hal::BatteryManagementStatus& status) {
  if (lock_screen == nullptr || lock_screen != g_battery_card_owner) {
    return;
  }
  UpdateBatteryOverviewCard(g_battery_card, status);
}

void SetLockScreenDragOffset(lv_obj_t* lock_screen, int offset) {
  if (lock_screen == nullptr) {
    return;
  }

  const int clamped_offset = std::min(offset, 0);
  lv_anim_delete(lock_screen, SetObjectY);
  lv_obj_set_y(lock_screen, clamped_offset);
}

void StartLockScreenResetAnimation(lv_obj_t* lock_screen) {
  if (lock_screen == nullptr) {
    return;
  }

  lv_anim_t position_animation;
  lv_anim_init(&position_animation);
  lv_anim_set_var(&position_animation, lock_screen);
  lv_anim_set_values(&position_animation, lv_obj_get_y(lock_screen), 0);
  lv_anim_set_duration(&position_animation, kLockResetAnimationMs);
  lv_anim_set_path_cb(&position_animation, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&position_animation, SetObjectY);
  lv_anim_start(&position_animation);
}

void StartLockScreenUnlockAnimation(lv_obj_t* lock_screen) {
  if (lock_screen == nullptr) {
    return;
  }

  lv_anim_t position_animation;
  lv_anim_init(&position_animation);
  lv_anim_set_var(&position_animation, lock_screen);
  lv_anim_set_values(&position_animation, lv_obj_get_y(lock_screen),
      -lv_obj_get_height(lock_screen));
  lv_anim_set_duration(&position_animation, kLockUnlockAnimationMs);
  lv_anim_set_path_cb(&position_animation, lv_anim_path_ease_out);
  lv_anim_set_exec_cb(&position_animation, SetObjectY);
  lv_anim_start(&position_animation);
}

}  // namespace lilygo_box::ui
