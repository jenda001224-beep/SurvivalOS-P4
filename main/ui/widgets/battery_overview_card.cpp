/*
 * @Description: Battery overview card (remaining time and charge level)
 * @License: GPL 3.0
 */
#include "ui/widgets/battery_overview_card.h"

#include <algorithm>
#include <cstdio>

#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"

namespace lilygo_box::ui {
namespace {

// Same geometry and colours as the card on the battery settings page.
constexpr int kCardInnerPadding = 28;
constexpr int kCardRadius = 40;
constexpr int kMainTextTop = 42;
constexpr int kStatusTextTop = 128;
constexpr int kStatusIconTop = kStatusTextTop - 2;
constexpr int kStatusChargingTextLeft = kCardInnerPadding + 32;
constexpr int kLowThresholdPercent = 20;
constexpr uint32_t kNormalColor = 0x35D66B;
constexpr uint32_t kChargingColor = 0x27C769;
constexpr uint32_t kLowColor = 0xFF3B30;
constexpr uint32_t kRestColor = 0xB8EFC8;
constexpr uint32_t kLowRestColor = 0xF6B2AE;
constexpr uint32_t kTextColor = 0xFFFFFF;

uint32_t FillColor(int percent, bool charging) {
  if (charging) {
    return kChargingColor;
  }
  if (percent >= 0 && percent < kLowThresholdPercent) {
    return kLowColor;
  }
  return kNormalColor;
}

uint32_t RestColor(int percent, bool charging) {
  if (!charging && percent >= 0 && percent < kLowThresholdPercent) {
    return kLowRestColor;
  }
  return kRestColor;
}

void FormatDuration(int minutes, char* buffer, size_t buffer_size) {
  if (minutes <= 0 || minutes >= 24 * 60 * 30) {
    std::snprintf(buffer, buffer_size, "-- h -- min");
    return;
  }
  std::snprintf(
      buffer, buffer_size, "%d h %02d min", minutes / 60, minutes % 60);
}

void FormatSummary(const hal::BatteryManagementStatus& status, char* buffer,
    size_t buffer_size) {
  if (!status.pack_present) {
    std::snprintf(buffer, buffer_size, "Battery not detected");
    return;
  }
  if (status.full_charged) {
    std::snprintf(buffer, buffer_size, "Fully charged | Battery %d%%",
        status.charge_percent);
    return;
  }
  if (status.charging) {
    std::snprintf(buffer, buffer_size,
        status.capabilities.remaining_time
            ? "Charging time estimate | Battery %d%%"
            : "Charging | Battery %d%%",
        status.charge_percent);
    return;
  }
  std::snprintf(buffer, buffer_size,
      status.capabilities.remaining_time ? "Available time | Battery %d%%"
                                         : "Battery %d%%",
      status.charge_percent);
}

/**
 * @brief Creates a plain box with no border, padding or scrolling.
 * @param parent Parent object.
 * @param width Box width.
 * @param color Background colour.
 * @param radius Corner radius.
 * @return The box, or nullptr.
 */
lv_obj_t* CreateBox(lv_obj_t* parent, int width, uint32_t color, int radius) {
  lv_obj_t* box = lv_obj_create(parent);
  if (box == nullptr) {
    return nullptr;
  }
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(box, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_set_size(box, width, kBatteryOverviewCardHeight);
  lv_obj_set_style_radius(box, radius, LV_PART_MAIN);
  lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(box, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
  return box;
}

lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text,
    const lv_font_t* font, int x, int y) {
  lv_obj_t* label = lv_label_create(parent);
  if (label == nullptr) {
    return nullptr;
  }
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(kTextColor), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  lv_obj_set_pos(label, x, y);
  return label;
}

}  // namespace

bool CreateBatteryOverviewCard(
    lv_obj_t* parent, int width, BatteryOverviewCard* card) {
  if (parent == nullptr || card == nullptr || width <= 0) {
    return false;
  }
  *card = {};
  card->width = width;

  card->card = CreateBox(parent, width, kRestColor, kCardRadius);
  if (card->card == nullptr) {
    return false;
  }
  lv_obj_set_style_clip_corner(card->card, true, LV_PART_MAIN);

  card->fill = CreateBox(card->card, width, kNormalColor, 0);
  card->time_label = CreateLabel(card->card, "-- h -- min",
      &lvgl_font_google_sans_flex_64, kCardInnerPadding, kMainTextTop);
  card->status_icon_label = CreateLabel(card->card, icon::kBolt,
      &lvgl_font_material_symbols_fill_32, kCardInnerPadding, kStatusIconTop);
  card->status_label = CreateLabel(card->card, "Battery --%",
      &lvgl_font_google_sans_flex_24, kCardInnerPadding, kStatusTextTop);
  if (card->fill == nullptr || card->time_label == nullptr ||
      card->status_icon_label == nullptr || card->status_label == nullptr) {
    lv_obj_delete(card->card);
    *card = {};
    return false;
  }
  lv_obj_set_pos(card->fill, 0, 0);
  lv_obj_add_flag(card->status_icon_label, LV_OBJ_FLAG_HIDDEN);
  return true;
}

void UpdateBatteryOverviewCard(const BatteryOverviewCard& card,
    const hal::BatteryManagementStatus& status) {
  if (card.card == nullptr || card.fill == nullptr ||
      card.time_label == nullptr || card.status_label == nullptr) {
    return;
  }

  const int percent = std::clamp(status.charge_percent, 0, 100);
  lv_obj_set_style_bg_color(card.card,
      lv_color_hex(RestColor(percent, status.charging)), LV_PART_MAIN);
  lv_obj_set_width(card.fill, std::max(1, card.width * percent / 100));
  lv_obj_set_style_bg_color(card.fill,
      lv_color_hex(FillColor(percent, status.charging)), LV_PART_MAIN);

  if (status.capabilities.remaining_time) {
    char time_text[32] = {};
    FormatDuration(
        status.charging ? status.time_to_full_min : status.time_to_empty_min,
        time_text, sizeof(time_text));
    lv_label_set_text(card.time_label, time_text);
    lv_obj_remove_flag(card.time_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(card.time_label, LV_OBJ_FLAG_HIDDEN);
  }

  char summary[80] = {};
  FormatSummary(status, summary, sizeof(summary));
  lv_label_set_text(card.status_label, summary);
  if (card.status_icon_label != nullptr) {
    const bool show_bolt = status.charging && !status.full_charged;
    if (show_bolt) {
      lv_obj_remove_flag(card.status_icon_label, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(card.status_icon_label, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_x(card.status_label,
        show_bolt ? kStatusChargingTextLeft : kCardInnerPadding);
  }
}

}  // namespace lilygo_box::ui
