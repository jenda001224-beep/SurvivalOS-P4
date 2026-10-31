/*
 * @Description: Settings: SD Card. Free room and what takes it, like the
 *               storage page of a phone
 * @License: GPL 3.0
 */
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <new>

#include "app/storage_usage.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/views/settings/settings_basic_view_common.h"

namespace lilygo_box::ui {
namespace {

constexpr uint32_t kRefreshMs = 400;
constexpr int kLegendRowHeight = 58;
constexpr int kBarHeight = 26;
constexpr uint32_t kFreeColor = 0x48484A;

struct CardCategory {
  const char* name;
  uint32_t color;
};
// Order of the bar and the list. The last one, Other, is what is left over.
constexpr CardCategory kCategories[] = {
    {"Maps", 0x34C759},      {"Wikipedia", 0x8E8E93},
    {"Nature", 0x30B0C7},    {"Translator", 0x5E5CE6},
    {"Music", 0xFF375F},     {"Photos", 0xFF9F0A},
    {"Recently Deleted", 0xFFD60A}, {"Other", 0xAC8E68},
};
constexpr size_t kCategoryCount = sizeof(kCategories) / sizeof(kCategories[0]);

struct SdCardPage {
  hal::StorageProvider* storage = nullptr;
  lv_timer_t* timer = nullptr;
  lv_obj_t* headline = nullptr;
  lv_obj_t* detail = nullptr;
  lv_obj_t* bar = nullptr;
  lv_obj_t* segments[kCategoryCount] = {};
  lv_obj_t* values[kCategoryCount] = {};
  lv_obj_t* free_value = nullptr;
  lv_obj_t* system_value = nullptr;
  lv_obj_t* first_aid_value = nullptr;
  lv_obj_t* internal_value = nullptr;
  int bar_width = 0;
};

void FormatBytes(uint64_t bytes, char* out, size_t size) {
  const double value = static_cast<double>(bytes);
  if (bytes >= 1000ULL * 1000 * 1000) {
    std::snprintf(out, size, value >= 100e9 ? "%.0f GB" : "%.1f GB", value / 1e9);
  } else if (bytes >= 1000ULL * 1000) {
    std::snprintf(out, size, value >= 100e6 ? "%.0f MB" : "%.1f MB", value / 1e6);
  } else if (bytes >= 1000) {
    std::snprintf(out, size, "%.0f KB", value / 1e3);
  } else {
    std::snprintf(out, size, "%llu B", static_cast<unsigned long long>(bytes));
  }
}

void SetBytes(lv_obj_t* label, uint64_t bytes) {
  if (label != nullptr) {
    char text[24];
    FormatBytes(bytes, text, sizeof(text));
    lv_label_set_text(label, text);
  }
}

void Refresh(SdCardPage* page) {
  const app::StorageUsage usage = app::GetStorageUsage();
  char text[80];
  if (!usage.card_present) {
    lv_label_set_text(page->headline, usage.measuring ? "Reading the card..." : "No SD card");
    lv_label_set_text(page->detail, usage.measuring ? "" : "Maps, Wikipedia, plants and music live on the card.");
  } else {
    char used[24];
    char total[24];
    FormatBytes(usage.card_total - usage.card_free, used, sizeof(used));
    FormatBytes(usage.card_total, total, sizeof(total));
    std::snprintf(text, sizeof(text), "%s of %s used", used, total);
    lv_label_set_text(page->headline, text);
    char free_text[24];
    FormatBytes(usage.card_free, free_text, sizeof(free_text));
    std::snprintf(text, sizeof(text), usage.measuring ? "%s free  ·  measuring..." : "%s free", free_text);
    lv_label_set_text(page->detail, text);
  }
  const uint64_t used = usage.card_total > usage.card_free ? usage.card_total - usage.card_free : 0;
  const uint64_t known = usage.maps + usage.wikipedia + usage.nature + usage.translator +
      usage.music + usage.photos + usage.recently_deleted;
  const uint64_t sizes[kCategoryCount] = {usage.maps, usage.wikipedia, usage.nature,
      usage.translator, usage.music, usage.photos, usage.recently_deleted,
      used > known ? used - known : 0};
  int x = 0;
  for (size_t i = 0; i < kCategoryCount; ++i) {
    SetBytes(page->values[i], sizes[i]);
    int width = 0;
    if (usage.card_total > 0 && sizes[i] > 0) {
      // Anything on the card stays visible, however small next to the rest.
      width = std::max<int>(4, static_cast<int>(
          sizes[i] * static_cast<uint64_t>(page->bar_width) / usage.card_total));
    }
    width = std::min(width, page->bar_width - x);
    lv_obj_set_pos(page->segments[i], x, 0);
    lv_obj_set_width(page->segments[i], width);
    x += width;
  }
  SetBytes(page->free_value, usage.card_free);
  SetBytes(page->system_value, usage.system);
  SetBytes(page->first_aid_value, usage.first_aid);
  if (page->internal_value != nullptr) {
    char used_text[24];
    char total_text[24];
    FormatBytes(usage.internal_used, used_text, sizeof(used_text));
    FormatBytes(usage.internal_total, total_text, sizeof(total_text));
    std::snprintf(text, sizeof(text), "%s of %s", used_text, total_text);
    lv_label_set_text(page->internal_value, text);
  }
}

void RefreshTimer(lv_timer_t* timer) {
  auto* page = static_cast<SdCardPage*>(lv_timer_get_user_data(timer));
  Refresh(page);
  if (!app::GetStorageUsage().measuring) {
    lv_timer_pause(timer);  // nothing changes any more
  }
}

void PageDeleted(lv_event_t* event) {
  auto* page = static_cast<SdCardPage*>(lv_event_get_user_data(event));
  if (page != nullptr) {
    if (page->timer != nullptr) {
      lv_timer_delete(page->timer);
    }
    delete page;
  }
}

lv_obj_t* LegendRow(lv_obj_t* body, const char* name, uint32_t color, int y, int width) {
  lv_obj_t* dot = lv_obj_create(body);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(dot, 18, 18);
  lv_obj_set_pos(dot, kBasicSidePadding, y + (kLegendRowHeight - 18) / 2);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(dot, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_t* label = CreateLabel(body, name, lv_color_hex(SettingsThemeColors().on_surface), Font28());
  lv_obj_set_pos(label, kBasicSidePadding + 34, y + (kLegendRowHeight - lv_font_get_line_height(Font28())) / 2);
  lv_obj_t* value = CreateLabel(body, "-", lv_color_hex(SettingsThemeColors().on_surface_variant), Font24());
  lv_obj_set_width(value, 220);
  lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
  lv_obj_set_pos(value, width - kBasicSidePadding - 220, y + (kLegendRowHeight - lv_font_get_line_height(Font24())) / 2);
  return value;
}

bool BuildSdCardContent(lv_obj_t* body, SettingsViewState* state) {
  auto* page = new (std::nothrow) SdCardPage();
  if (page == nullptr) {
    return false;
  }
  lv_obj_add_event_cb(body, PageDeleted, LV_EVENT_DELETE, page);
  page->storage = state->config.storage;
  const int width = state->config.width;
  const int inner = width - 2 * kBasicSidePadding;
  int y = 8;
  page->headline = CreateLabel(body, "", lv_color_hex(SettingsThemeColors().on_surface), Font32());
  lv_obj_set_pos(page->headline, kBasicSidePadding, y);
  y += lv_font_get_line_height(Font32()) + 2;
  page->detail = CreateLabel(body, "", lv_color_hex(SettingsThemeColors().on_surface_variant), Font24());
  lv_obj_set_width(page->detail, inner);
  lv_label_set_long_mode(page->detail, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(page->detail, kBasicSidePadding, y);
  y += lv_font_get_line_height(Font24()) + 16;

  page->bar = lv_obj_create(body);
  lv_obj_remove_flag(page->bar, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(page->bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(page->bar, inner, kBarHeight);
  lv_obj_set_pos(page->bar, kBasicSidePadding, y);
  lv_obj_set_style_radius(page->bar, kBarHeight / 2, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(page->bar, true, LV_PART_MAIN);
  lv_obj_set_style_border_width(page->bar, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(page->bar, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(page->bar, lv_color_hex(kFreeColor), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page->bar, LV_OPA_COVER, LV_PART_MAIN);
  page->bar_width = inner;
  for (size_t i = 0; i < kCategoryCount; ++i) {
    lv_obj_t* segment = lv_obj_create(page->bar);
    lv_obj_remove_flag(segment, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(segment, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(segment, 0, kBarHeight);
    lv_obj_set_style_radius(segment, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(segment, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(segment, lv_color_hex(kCategories[i].color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(segment, LV_OPA_COVER, LV_PART_MAIN);
    page->segments[i] = segment;
  }
  y += kBarHeight + 14;
  for (size_t i = 0; i < kCategoryCount; ++i) {
    page->values[i] = LegendRow(body, kCategories[i].name, kCategories[i].color, y, width);
    y += kLegendRowHeight;
  }
  page->free_value = LegendRow(body, "Free", kFreeColor, y, width);
  y += kLegendRowHeight + 12;

  if (!CreateSectionLabel(body, "Phone memory", y, width)) {
    return false;
  }
  y += kBasicSectionHeight;
  page->system_value = LegendRow(body, "SurvivalOS", 0x0A84FF, y, width);
  y += kLegendRowHeight;
  page->first_aid_value = LegendRow(body, "First Aid", 0xFF453A, y, width);
  y += kLegendRowHeight;
  page->internal_value = LegendRow(body, "Saved data", 0x8E8E93, y, width);
  y += kLegendRowHeight + 30;
  // Keeps the last row clear of the bottom edge when scrolled to the end.
  lv_obj_t* end = lv_obj_create(body);
  lv_obj_remove_flag(end, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(end, 1, 1);
  lv_obj_set_style_bg_opa(end, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(end, 0, LV_PART_MAIN);
  lv_obj_set_pos(end, 0, y);

  app::StartStorageUsageScan(page->storage);
  Refresh(page);
  page->timer = lv_timer_create(RefreshTimer, kRefreshMs, page);
  return true;
}

}  // namespace

bool ShowSdCardPage(SettingsViewState* state) {
  return ShowBasicPage(state, "SD Card", BuildSdCardContent);
}

}  // namespace lilygo_box::ui
