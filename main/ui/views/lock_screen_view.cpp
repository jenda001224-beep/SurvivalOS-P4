/*
 * @Description: 锁屏时钟布局、拖动回弹与解锁动画实现
 * @Author: LILYGO_L
 * @Date: 2026-05-10 13:27:05
 * @LastEditTime: 2026-09-02 17:54:35
 * @License: GPL 3.0
 */
#include "ui/views/lock_screen_view.h"

#include <algorithm>

#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"
#include "ui/wallpaper.h"
#include "ui/widgets/battery_overview_card.h"

namespace lilygo_box::ui {
namespace {

constexpr int kLockClockTop = 90;
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

/**
 * @brief Forgets the battery card when its lock screen is deleted.
 * @param event LVGL delete event of the lock screen.
 */
void LockScreenDeleteEventCallback(lv_event_t* event) {
  if (lv_event_get_target_obj(event) == g_battery_card_owner) {
    g_battery_card = {};
    g_battery_card_owner = nullptr;
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
  lv_obj_align(group, LV_ALIGN_TOP_LEFT, kLockHorizontalPadding, kLockClockTop);

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

}  // namespace

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

  CreateWallpaperObjects(
      lock_screen, options.screen_width, options.screen_height);

  if (CreateBatteryGroup(lock_screen, options) == nullptr) {
    lv_obj_delete(lock_screen);
    return nullptr;
  }

  lv_obj_t* arrow = CreateLabel(lock_screen, icon::kKeyboardArrowUp,
      lv_color_hex(theme::FixedColors().lock_screen_text),
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
  // The lock screen no longer shows a clock (see CreateBatteryGroup). The
  // function stays so the callers upstream has need no change.
  (void)lock_screen;
  (void)time_text;
  (void)date_text;
  (void)week_text;
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
