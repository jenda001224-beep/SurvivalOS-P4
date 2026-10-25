/*
 * @Description: Keeps scrolling labels at least one line tall
 * @License: GPL 3.0
 */
#pragma once

#include "lvgl.h"

namespace lilygo_box::ui {

/**
 * @brief SurvivalOS: makes a label at least as tall as one line of its font.
 *
 * The text fonts carry Czech and other accented capitals since 2026-10, which
 * made their lines taller (28 px: 29 -> 38). A scrolling label (SCROLL or
 * SCROLL_CIRCULAR) shorter than one line scrolls its single line vertically,
 * out of sight, so labels sized for the old fonts showed blank names.
 * Call after setting the label's font and size.
 */
inline void EnsureLabelLineHeight(lv_obj_t* label) {
  if (label == nullptr) {
    return;
  }
  const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
  if (font == nullptr) {
    return;
  }
  // The height as set (no layout pass): content and percent sizes are left alone.
  const int32_t height = lv_obj_get_style_height(label, LV_PART_MAIN);
  const int32_t line_height = lv_font_get_line_height(font);
  if (!LV_COORD_IS_SPEC(height) && height < line_height) {
    lv_obj_set_height(label, line_height);
  }
}

}  // namespace lilygo_box::ui
