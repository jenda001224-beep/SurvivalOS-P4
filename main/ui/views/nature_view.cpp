/*
 * @Description: Field guide of the plants and fungi of Czechia
 * @License: GPL 3.0
 *
 * Reads nature.svnat (tools/prepare_nature.py) from /nature on the SD card.
 * The list is virtual: a dozen row widgets are moved and refilled while
 * scrolling, so thousands of species cost no more than one screen of rows.
 * Thumbnails are decoded one per timer tick, and only while the list is not
 * moving, so scrolling never waits for the JPEG decoder.
 */
#include "ui/views/nature_view.h"

#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "hal/providers/rtc_provider.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "hal/providers/storage_provider.h"
#include "survival/jpeg_image.h"
#include "survival/nature_pack.h"
#include "ui/haptic_feedback.h"
#include "ui/input/back_navigation_controller.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/survival_palette.h"
#include "ui/theme/theme_provider.h"
#include "ui/widgets/shared_keyboard.h"

namespace lilygo_box::ui {
namespace {

using survival::NaturePack;
using survival::NatureSpecies;

constexpr const char* kNatureDirectory = "nature";
constexpr int kSidePadding = 18;
constexpr int kTitleTop = 58;
constexpr int kSearchTop = 112;
constexpr int kSearchHeight = 68;
constexpr int kChipRowHeight = 52;
constexpr int kChipGap = 10;
constexpr int kRowHeight = 132;
constexpr int kThumbSize = 108;
constexpr int kRowPool = 14;
constexpr int kPhotoHeightPercent = 78;
constexpr uint32_t kThumbTickMs = 40;
constexpr int64_t kScrollQuietUs = 160 * 1000;
constexpr int kMaxQuery = 48;

// Survival Mode draws the guide as thin red text on black (an AMOLED pixel
// that is off costs nothing); photos stay in colour, they are the point.
uint32_t Pick(uint32_t normal, uint32_t survival) {
  return ui::survival_look::Active() ? survival : normal;
}
uint32_t Accent() { return Pick(0x4CAF50, ui::survival_look::kText); }
uint32_t ChipColor() { return Pick(0x2C2C2E, ui::survival_look::kBlack); }
uint32_t ChipText() { return Pick(0xE5E5EA, ui::survival_look::kText); }
uint32_t Muted() { return Pick(0x8E8E93, ui::survival_look::kDim); }
uint32_t Placeholder() { return Pick(0x1C1C1E, ui::survival_look::kFaint); }
uint32_t Primary() { return Pick(0xFFFFFF, ui::survival_look::kText); }

struct ColorInfo {
  const char* name;
  uint32_t rgb;
};
constexpr ColorInfo kColors[survival::kNatureColorCount] = {
    {"White", 0xF2F2F2}, {"Yellow", 0xF5C842}, {"Orange", 0xF08A24},
    {"Red", 0xD93B30}, {"Pink", 0xF07AAE}, {"Purple", 0x8E5BD6},
    {"Blue", 0x3D7BE0}, {"Green", 0x4CAF50}, {"Brown", 0x8B5A2B},
    {"Grey", 0x8E8E93},
};

struct GroupFilter {
  const char* label;
  uint32_t groups;  // bit per NatureGroup
};
constexpr GroupFilter kGroupFilters[] = {
    {"All", 0xFF}, {"Flowers", 1u << 0}, {"Trees", (1u << 1) | (1u << 3)},
    {"Ferns & mosses", (1u << 2) | (1u << 4)}, {"Mushrooms", 1u << 5},
    {"Lichens & other", (1u << 6) | (1u << 7)},
};

struct UseFilter {
  const char* label;
  uint8_t mask;
  uint32_t color;
};
constexpr UseFilter kUseFilters[] = {
    {"Edible", survival::kNatureEdible, 0x30D158},
    {"Medicinal", survival::kNatureMedicinal, 0x64A8FF},
    {"Poisonous", survival::kNaturePoisonous, 0xFF453A},
};

struct HabitatFilter {
  const char* label;
  uint16_t mask;
};
constexpr HabitatFilter kHabitats[] = {
    {"Forest", survival::kNatureForest}, {"Meadow", survival::kNatureMeadow},
    {"Water", survival::kNatureWater}, {"Rocks", survival::kNatureRocks},
    {"Fields", survival::kNatureFields}, {"Gardens", survival::kNatureGardens},
    {"On wood", survival::kNatureWood}, {"Mountains", survival::kNatureMountains},
};

constexpr const char* kMonthLetters[12] = {"J", "F", "M", "A", "M", "J", "J", "A",
    "S", "O", "N", "D"};
constexpr const char* kMonthNames[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

struct Row {
  lv_obj_t* box = nullptr;
  lv_obj_t* thumb = nullptr;
  lv_obj_t* name = nullptr;
  lv_obj_t* latin = nullptr;
  lv_obj_t* info = nullptr;
  int64_t bound = -1;           // list position shown
  uint32_t species = UINT32_MAX;
  survival::JpegImage image;
  lv_image_dsc_t descriptor = {};
  bool has_image = false;
};

struct NatureViewState {
  hal::StorageProvider* storage = nullptr;
  hal::RtcProvider* rtc = nullptr;
  int width = 0;
  int height = 0;
  lv_obj_t* root = nullptr;
  lv_obj_t* count_label = nullptr;
  lv_obj_t* search_field = nullptr;
  lv_obj_t* keyboard = nullptr;
  lv_obj_t* list = nullptr;
  lv_obj_t* spacer = nullptr;
  lv_obj_t* empty_label = nullptr;
  lv_obj_t* group_chips[sizeof(kGroupFilters) / sizeof(kGroupFilters[0])] = {};
  lv_obj_t* use_chips[sizeof(kUseFilters) / sizeof(kUseFilters[0])] = {};
  lv_obj_t* season_chip = nullptr;
  lv_obj_t* sort_chip = nullptr;
  lv_obj_t* color_chips[survival::kNatureColorCount] = {};
  lv_obj_t* habitat_chips[sizeof(kHabitats) / sizeof(kHabitats[0])] = {};
  Row rows[kRowPool];
  lv_timer_t* thumb_timer = nullptr;
  int64_t last_scroll_us = 0;

  NaturePack pack;
  bool loaded = false;
  uint32_t* order_by_name = nullptr;   // 0..n-1, the pack's own order
  uint32_t* order_by_count = nullptr;  // most observed first
  uint32_t* filtered = nullptr;
  uint32_t filtered_count = 0;

  int group = 0;
  uint8_t uses = 0;
  int color = -1;
  uint16_t habitats = 0;
  bool in_season = false;
  bool by_count = false;
  char query[kMaxQuery + 1] = {};
  int current_month = -1;

  // Detail page.
  lv_obj_t* detail = nullptr;
  survival::JpegImage photos[4];
  lv_image_dsc_t photo_descriptors[4] = {};
  int photo_count = 0;
  lv_obj_t* photo_credit = nullptr;
  lv_obj_t* photo_dots[4] = {};
  uint32_t detail_species = 0;
};

const theme::ThemeColors& Colors() { return theme::ActiveThemeColors(); }
uint32_t Surface() { return Pick(Colors().surface, ui::survival_look::kBlack); }
uint32_t OnSurface() { return Pick(Colors().on_surface, ui::survival_look::kText); }

void MakePlain(lv_obj_t* object) {
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(object, 0, LV_PART_MAIN);
}

lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, uint32_t color,
    const lv_font_t* font) {
  lv_obj_t* label = lv_label_create(parent);
  if (label != nullptr) {
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  }
  return label;
}

void FillDescriptor(lv_image_dsc_t* descriptor, const survival::JpegImage& image) {
  std::memset(descriptor, 0, sizeof(*descriptor));
  descriptor->header.magic = LV_IMAGE_HEADER_MAGIC;
  descriptor->header.cf = LV_COLOR_FORMAT_RGB565;
  descriptor->header.w = image.width;
  descriptor->header.h = image.height;
  descriptor->header.stride = image.width * sizeof(uint16_t);
  descriptor->data = image.pixels;
  descriptor->data_size = image.width * image.height * sizeof(uint16_t);
}

// Short "Edible · Forest, Meadow · May–Oct" line for a list row.
void DescribeShort(const NatureSpecies& species, char* out, size_t size,
    uint32_t* color) {
  size_t used = 0;
  *color = Muted();
  auto add = [&](const char* text) {
    if (used > 0 && used + 3 < size) {
      used += std::snprintf(out + used, size - used, " · ");
    }
    if (used < size) {
      used += std::snprintf(out + used, size - used, "%s", text);
    }
  };
  if (species.uses & survival::kNatureDeadly) {
    add("Deadly"); *color = 0xFF453A;
  } else if (species.uses & survival::kNaturePoisonous) {
    add("Poisonous"); *color = 0xFF6B5E;
  } else if (species.uses & survival::kNatureEdible) {
    add("Edible"); *color = 0x30D158;
  } else if (species.uses & survival::kNatureMedicinal) {
    add("Medicinal"); *color = 0x64A8FF;
  }
  for (const HabitatFilter& habitat : kHabitats) {
    if (species.habitats & habitat.mask) {
      add(habitat.label);
      break;
    }
  }
  int first = -1, last = -1;
  for (int month = 0; month < 12; ++month) {
    if (species.months & (1u << month)) {
      if (first < 0) first = month;
      last = month;
    }
  }
  if (first >= 0) {
    char months[16];
    std::snprintf(months, sizeof(months), "%s–%s", kMonthNames[first], kMonthNames[last]);
    add(first == last ? kMonthNames[first] : months);
  }
}

// ── Filtering ────────────────────────────────────────────────────────────────

bool Matches(const NatureViewState* state, uint32_t index, const char* folded_query) {
  const NatureSpecies& species = state->pack.Species(index);
  if (!(kGroupFilters[state->group].groups & (1u << species.group))) {
    return false;
  }
  if (state->uses != 0 && (species.uses & state->uses) == 0) {
    return false;
  }
  if (state->color >= 0 && species.color != state->color) {
    return false;
  }
  if (state->habitats != 0 && (species.habitats & state->habitats) == 0) {
    return false;
  }
  if (state->in_season && state->current_month >= 0 &&
      !(species.months & (1u << state->current_month))) {
    return false;
  }
  return folded_query[0] == '\0' ||
      std::strstr(state->pack.SearchKey(index), folded_query) != nullptr;
}

void LayoutList(NatureViewState* state, bool force);

void ApplyFilters(NatureViewState* state) {
  if (!state->loaded) {
    return;
  }
  char folded[kMaxQuery * 2 + 1] = {};
  survival::FoldForSearch(state->query, folded, sizeof(folded));
  const uint32_t* order = state->by_count ? state->order_by_count : state->order_by_name;
  uint32_t count = 0;
  for (uint32_t i = 0; i < state->pack.SpeciesCount(); ++i) {
    if (Matches(state, order[i], folded)) {
      state->filtered[count++] = order[i];
    }
  }
  state->filtered_count = count;
  char text[48];
  std::snprintf(text, sizeof(text), "%lu of %lu", static_cast<unsigned long>(count),
      static_cast<unsigned long>(state->pack.SpeciesCount()));
  lv_label_set_text(state->count_label, text);
  if (state->empty_label != nullptr) {
    if (count == 0) {
      lv_obj_remove_flag(state->empty_label, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(state->empty_label, LV_OBJ_FLAG_HIDDEN);
    }
  }
  lv_obj_set_height(state->spacer, std::max<int32_t>(1, count * kRowHeight));
  lv_obj_scroll_to_y(state->list, 0, LV_ANIM_OFF);
  LayoutList(state, true);
}

// ── Virtual list ─────────────────────────────────────────────────────────────

void ReleaseRowImage(Row* row) {
  if (row->has_image) {
    lv_image_set_src(row->thumb, nullptr);
    lv_image_cache_drop(&row->descriptor);
    survival::FreeJpegImage(&row->image);
    row->has_image = false;
  }
}

void BindRow(NatureViewState* state, Row* row, int64_t position) {
  if (position < 0 || position >= state->filtered_count) {
    lv_obj_add_flag(row->box, LV_OBJ_FLAG_HIDDEN);
    row->bound = -1;
    return;
  }
  lv_obj_remove_flag(row->box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_y(row->box, static_cast<int32_t>(position * kRowHeight));
  const uint32_t species_index = state->filtered[position];
  if (row->bound == position && row->species == species_index) {
    return;
  }
  row->bound = position;
  if (row->species != species_index) {
    ReleaseRowImage(row);
    row->species = species_index;
  }
  const NatureSpecies& species = state->pack.Species(species_index);
  lv_label_set_text(row->name, state->pack.Name(species.name_cs));
  lv_label_set_text(row->latin, state->pack.Name(species.name_lat));
  char info[96] = {};
  uint32_t color = Muted();
  DescribeShort(species, info, sizeof(info), &color);
  lv_label_set_text(row->info, info);
  lv_obj_set_style_text_color(row->info, lv_color_hex(color), LV_PART_MAIN);
}

void LayoutList(NatureViewState* state, bool force) {
  const int32_t scroll = lv_obj_get_scroll_y(state->list);
  const int64_t first = std::max<int64_t>(0, scroll / kRowHeight - 1);
  for (int i = 0; i < kRowPool; ++i) {
    Row* row = &state->rows[i];
    // Each pool slot keeps the same position modulo the pool size, so rows
    // that stay on screen are not refilled.
    int64_t position = first + ((i - first) % kRowPool + kRowPool) % kRowPool;
    if (force) {
      row->bound = -1;
    }
    BindRow(state, row, position);
  }
}

void ListScrollCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  state->last_scroll_us = esp_timer_get_time();
  LayoutList(state, false);
}

// Fills in at most one missing thumbnail per tick, never while scrolling.
void ThumbTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<NatureViewState*>(lv_timer_get_user_data(timer));
  if (state == nullptr || !state->loaded || state->detail != nullptr ||
      esp_timer_get_time() - state->last_scroll_us < kScrollQuietUs) {
    return;
  }
  const int32_t top = lv_obj_get_scroll_y(state->list);
  const int32_t bottom = top + lv_obj_get_height(state->list);
  for (Row& row : state->rows) {
    if (row.bound < 0 || row.has_image) {
      continue;
    }
    const int32_t y = static_cast<int32_t>(row.bound * kRowHeight);
    if (y + kRowHeight < top || y > bottom) {
      continue;
    }
    const NatureSpecies& species = state->pack.Species(row.species);
    size_t size = 0;
    uint8_t* jpeg = state->pack.ReadPhoto(species.first_photo, &size);
    if (jpeg != nullptr && survival::DecodeJpegBuffer(jpeg, size, kThumbSize * 2,
                               kThumbSize * 2, &row.image)) {
      FillDescriptor(&row.descriptor, row.image);
      lv_image_set_src(row.thumb, &row.descriptor);
      // Cover the square the way a photo app crops a thumbnail.
      const uint32_t shorter = std::min(row.image.width, row.image.height);
      lv_image_set_scale(row.thumb, shorter > 0 ? 256 * kThumbSize / shorter : 256);
      lv_image_set_inner_align(row.thumb, LV_IMAGE_ALIGN_CENTER);
      row.has_image = true;
    } else {
      row.has_image = true;  // do not retry a broken photo every tick
    }
    heap_caps_free(jpeg);
    return;
  }
}

// ── Detail page ───────────────────────────────────────────────────────────────

void CloseDetail(NatureViewState* state) {
  if (state->detail == nullptr) {
    return;
  }
  lv_obj_delete(state->detail);
  state->detail = nullptr;
  for (int i = 0; i < state->photo_count; ++i) {
    lv_image_cache_drop(&state->photo_descriptors[i]);
    survival::FreeJpegImage(&state->photos[i]);
  }
  state->photo_count = 0;
}

void BackButtonCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  PlayUiHapticEffect(HapticEffect::kSelection);
  CloseDetail(state);
}

void PhotoScrollCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  lv_obj_t* strip = lv_event_get_target_obj(event);
  if (state == nullptr || strip == nullptr || state->photo_count == 0) {
    return;
  }
  const int index = std::clamp(
      static_cast<int>((lv_obj_get_scroll_x(strip) + state->width / 2) / state->width), 0,
      state->photo_count - 1);
  for (int i = 0; i < state->photo_count; ++i) {
    if (state->photo_dots[i] != nullptr) {
      lv_obj_set_style_bg_opa(state->photo_dots[i], i == index ? LV_OPA_COVER : LV_OPA_40,
          LV_PART_MAIN);
    }
  }
  const NatureSpecies& species = state->pack.Species(state->detail_species);
  lv_label_set_text(state->photo_credit,
      state->pack.Name(state->pack.Photo(species.first_photo + index).credit));
}

lv_obj_t* AddPill(lv_obj_t* parent, const char* text, uint32_t color) {
  lv_obj_t* pill = lv_obj_create(parent);
  MakePlain(pill);
  lv_obj_set_size(pill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(pill, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(pill, 8, LV_PART_MAIN);
  lv_obj_set_style_radius(pill, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(pill, lv_color_hex(color), LV_PART_MAIN);
  if (survival_look::Active()) {
    // Edible and poisonous keep their colours: that difference matters most.
    lv_obj_set_style_bg_opa(pill, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(pill, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(pill, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_border_opa(pill, LV_OPA_60, LV_PART_MAIN);
  } else {
    lv_obj_set_style_bg_opa(pill, 46, LV_PART_MAIN);
  }
  CreateLabel(pill, text, color, &lvgl_font_google_sans_flex_22);
  return pill;
}

lv_obj_t* AddFlowRow(lv_obj_t* parent, int width) {
  lv_obj_t* row = lv_obj_create(parent);
  MakePlain(row);
  lv_obj_set_size(row, width, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_row(row, 10, LV_PART_MAIN);
  return row;
}

void AddText(lv_obj_t* parent, const char* text, int width) {
  // Plain-text extracts mark sections as "== Title =="; those become headings.
  const char* cursor = text;
  char line[1400];
  while (*cursor != '\0') {
    const char* end = std::strchr(cursor, '\n');
    const size_t length = end != nullptr ? static_cast<size_t>(end - cursor) : std::strlen(cursor);
    const size_t copy = std::min(length, sizeof(line) - 1);
    std::memcpy(line, cursor, copy);
    line[copy] = '\0';
    cursor += length + (end != nullptr ? 1 : 0);
    char* start = line;
    while (*start == ' ') ++start;
    if (*start == '\0') {
      continue;
    }
    const bool heading = start[0] == '=';
    if (heading) {
      while (*start == '=' || *start == ' ') ++start;
      char* tail = start + std::strlen(start);
      while (tail > start && (tail[-1] == '=' || tail[-1] == ' ')) *--tail = '\0';
      // Literature and links are of no use without a network.
      if (strcasecmp(start, "Odkazy") == 0 || strcasecmp(start, "Reference") == 0 ||
          strcasecmp(start, "Literatura") == 0 || strcasecmp(start, "Externí odkazy") == 0 ||
          strcasecmp(start, "Související články") == 0 || strcasecmp(start, "References") == 0 ||
          strcasecmp(start, "External links") == 0) {
        break;
      }
    }
    lv_obj_t* label = CreateLabel(parent, start,
        heading ? OnSurface() : Pick(0xD1D1D6, ui::survival_look::kDim),
        heading ? &lvgl_font_google_sans_flex_28 : &lvgl_font_google_sans_flex_24);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(label, heading ? 0 : 5, LV_PART_MAIN);
    if (heading) {
      lv_obj_set_style_pad_top(label, 14, LV_PART_MAIN);
    }
  }
}

void OpenDetail(NatureViewState* state, uint32_t species_index) {
  CloseDetail(state);
  state->detail_species = species_index;
  const NatureSpecies& species = state->pack.Species(species_index);
  const int width = state->width;
  const int content_width = width - 2 * kSidePadding;

  lv_obj_t* page = lv_obj_create(state->root);
  state->detail = page;
  MakePlain(page);
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(page, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_size(page, width, state->height);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(Surface()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(page, 60, LV_PART_MAIN);
  lv_obj_set_style_pad_row(page, 14, LV_PART_MAIN);

  // Photos, swiped sideways.
  const int photo_height = width * kPhotoHeightPercent / 100;
  lv_obj_t* strip = lv_obj_create(page);
  MakePlain(strip);
  lv_obj_add_flag(strip, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(strip, width, photo_height);
  lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
  lv_obj_set_scroll_snap_x(strip, LV_SCROLL_SNAP_START);
  lv_obj_add_flag(strip, LV_OBJ_FLAG_SCROLL_ONE);
  lv_obj_set_scrollbar_mode(strip, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_style_bg_color(strip, lv_color_hex(Placeholder()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_event_cb(strip, PhotoScrollCallback, LV_EVENT_SCROLL_END, state);
  state->photo_count = std::min<int>(species.photo_count, 4);
  for (int i = 0; i < state->photo_count; ++i) {
    lv_obj_t* frame = lv_obj_create(strip);
    MakePlain(frame);
    lv_obj_add_flag(frame, LV_OBJ_FLAG_SNAPPABLE);
    lv_obj_set_size(frame, width, photo_height);
    lv_obj_set_pos(frame, i * width, 0);
    lv_obj_set_style_clip_corner(frame, true, LV_PART_MAIN);
    size_t size = 0;
    uint8_t* jpeg = state->pack.ReadPhoto(species.first_photo + i, &size);
    if (jpeg != nullptr && survival::DecodeJpegBuffer(jpeg, size, width * 2,
                               photo_height * 2, &state->photos[i])) {
      FillDescriptor(&state->photo_descriptors[i], state->photos[i]);
      lv_obj_t* image = lv_image_create(frame);
      lv_image_set_src(image, &state->photo_descriptors[i]);
      // Fill the frame, cropping the longer side.
      const uint32_t scale_w = 256u * width / std::max<uint32_t>(1, state->photos[i].width);
      const uint32_t scale_h = 256u * photo_height / std::max<uint32_t>(1, state->photos[i].height);
      lv_image_set_scale(image, std::max(scale_w, scale_h));
      lv_obj_set_size(image, width, photo_height);
      lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
      lv_obj_center(image);
    }
    heap_caps_free(jpeg);
  }

  lv_obj_t* back = lv_button_create(page);
  lv_obj_add_flag(back, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(back, 76, 76);
  lv_obj_set_pos(back, 18, 60);
  lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(back, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(back, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(back, BackButtonCallback, LV_EVENT_CLICKED, state);
  lv_obj_t* arrow = CreateLabel(back, icon::kArrowBack, Primary(),
      &lvgl_font_material_symbols_fill_32);
  lv_obj_center(arrow);

  // Dots and the photographer's credit.
  lv_obj_t* under = lv_obj_create(page);
  MakePlain(under);
  lv_obj_set_size(under, content_width, LV_SIZE_CONTENT);
  lv_obj_set_style_margin_left(under, kSidePadding, LV_PART_MAIN);
  lv_obj_t* dots = lv_obj_create(under);
  MakePlain(dots);
  lv_obj_set_size(dots, LV_SIZE_CONTENT, 12);
  lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(dots, 8, LV_PART_MAIN);
  for (int i = 0; i < state->photo_count; ++i) {
    lv_obj_t* dot = lv_obj_create(dots);
    MakePlain(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, i == 0 ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    state->photo_dots[i] = dot;
  }
  state->photo_credit = CreateLabel(under,
      state->photo_count > 0 ? state->pack.Name(state->pack.Photo(species.first_photo).credit) : "",
      Muted(), &lvgl_font_google_sans_flex_22);
  lv_obj_set_width(state->photo_credit, content_width);
  lv_label_set_long_mode(state->photo_credit, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(state->photo_credit, 0, 22);

  lv_obj_t* body = lv_obj_create(page);
  MakePlain(body);
  lv_obj_set_size(body, content_width, LV_SIZE_CONTENT);
  lv_obj_set_style_margin_left(body, kSidePadding, LV_PART_MAIN);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 12, LV_PART_MAIN);

  lv_obj_t* name = CreateLabel(body, state->pack.Name(species.name_cs), OnSurface(),
      &lvgl_font_google_sans_flex_36);
  lv_obj_set_width(name, content_width);
  lv_label_set_long_mode(name, LV_LABEL_LONG_WRAP);
  char latin[160];
  const char* english = state->pack.Name(species.name_en);
  std::snprintf(latin, sizeof(latin), "%s%s%s · %s", state->pack.Name(species.name_lat),
      english[0] ? " · " : "", english, state->pack.Name(species.family));
  lv_obj_t* latin_label = CreateLabel(body, latin, Muted(), &lvgl_font_google_sans_flex_24);
  lv_obj_set_width(latin_label, content_width);
  lv_label_set_long_mode(latin_label, LV_LABEL_LONG_WRAP);

  lv_obj_t* uses = AddFlowRow(body, content_width);
  struct UseLabel { uint8_t bit; const char* text; uint32_t color; };
  const UseLabel use_labels[] = {
      {survival::kNatureDeadly, "Deadly poisonous", 0xFF453A},
      {survival::kNaturePoisonous, "Poisonous", 0xFF6B5E},
      {survival::kNatureEdible, "Edible", 0x30D158},
      {survival::kNatureMedicinal, "Medicinal", 0x64A8FF},
      {survival::kNatureInedible, "Inedible", 0xAEAEB2},
      {survival::kNaturePsychoactive, "Psychoactive", 0xBF5AF2},
      {survival::kNatureInvasive, "Invasive", 0xFF9F0A},
      {survival::kNatureProtected, "Protected", 0x64D2FF},
  };
  bool any_use = false;
  for (const UseLabel& use : use_labels) {
    if ((species.uses & use.bit) &&
        !(use.bit == survival::kNaturePoisonous && (species.uses & survival::kNatureDeadly))) {
      AddPill(uses, use.text, use.color);
      any_use = true;
    }
  }
  if (!any_use) {
    AddPill(uses, "No known use", 0xAEAEB2);
  }
  if (species.uses & (survival::kNatureEdible | survival::kNatureMedicinal)) {
    lv_obj_t* warning = CreateLabel(body,
        "Only eat or use what you can identify with complete certainty. Many edible "
        "species have poisonous look-alikes.",
        0xFFD60A, &lvgl_font_google_sans_flex_22);
    lv_obj_set_width(warning, content_width);
    lv_label_set_long_mode(warning, LV_LABEL_LONG_WRAP);
  }

  // When it is found: one box per month.
  lv_obj_t* months = lv_obj_create(body);
  MakePlain(months);
  lv_obj_set_size(months, content_width, 64);
  const int cell = (content_width - 11 * 6) / 12;
  for (int month = 0; month < 12; ++month) {
    lv_obj_t* box = lv_obj_create(months);
    MakePlain(box);
    lv_obj_set_size(box, cell, 64);
    lv_obj_set_pos(box, month * (cell + 6), 0);
    lv_obj_set_style_radius(box, 10, LV_PART_MAIN);
    const bool on = species.months & (1u << month);
    const bool survival = survival_look::Active();
    lv_obj_set_style_bg_color(box,
        lv_color_hex(survival ? (on ? survival_look::kFaint : survival_look::kBlack)
                              : (on ? Accent() : ChipColor())),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(box, on || survival ? LV_OPA_COVER : LV_OPA_60, LV_PART_MAIN);
    if (month == state->current_month) {
      lv_obj_set_style_border_width(box, 3, LV_PART_MAIN);
      lv_obj_set_style_border_color(box, lv_color_hex(Primary()), LV_PART_MAIN);
    }
    lv_obj_t* letter = CreateLabel(box, kMonthLetters[month],
        survival ? (on ? survival_look::kText : survival_look::kDim)
                 : (on ? 0x0B2410 : Muted()),
        &lvgl_font_google_sans_flex_22);
    lv_obj_center(letter);
  }

  lv_obj_t* places = AddFlowRow(body, content_width);
  for (const HabitatFilter& habitat : kHabitats) {
    if (species.habitats & habitat.mask) {
      AddPill(places, habitat.label, 0xC7C7CC);
    }
  }
  if (species.color < survival::kNatureColorCount) {
    AddPill(places, kColors[species.color].name, kColors[species.color].rgb);
  }

  char* text = state->pack.ReadText(species);
  if (text != nullptr) {
    AddText(body, text, content_width);
    heap_caps_free(text);
  }
  lv_obj_t* credit = CreateLabel(body, state->pack.Name(species.credit), Muted(),
      &lvgl_font_google_sans_flex_22);
  lv_obj_set_width(credit, content_width);
  lv_label_set_long_mode(credit, LV_LABEL_LONG_WRAP);
}

void RowClickedCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  lv_obj_t* box = lv_event_get_current_target_obj(event);
  for (Row& row : state->rows) {
    if (row.box == box && row.bound >= 0) {
      PlayUiHapticEffect(HapticEffect::kImpactLight);
      if (state->keyboard != nullptr) {
        DefocusSharedKeyboardTextAreas();
      }
      OpenDetail(state, row.species);
      return;
    }
  }
}

// ── Chips ────────────────────────────────────────────────────────────────────

void StyleChip(lv_obj_t* chip, bool on, uint32_t accent = Accent()) {
  if (chip == nullptr) {
    return;
  }
  lv_obj_t* label = lv_obj_get_child(chip, 0);
  const bool is_label = label != nullptr && lv_obj_check_type(label, &lv_label_class);
  if (survival_look::Active()) {
    // Outline only: a lit fill would cost more than all the text together.
    lv_obj_set_style_bg_color(chip, lv_color_hex(survival_look::kBlack), LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip,
        lv_color_hex(on ? survival_look::kText : survival_look::kFaint), LV_PART_MAIN);
    if (is_label) {
      lv_obj_set_style_text_color(label,
          lv_color_hex(on ? survival_look::kText : survival_look::kDim), LV_PART_MAIN);
    }
    return;
  }
  lv_obj_set_style_bg_color(chip, lv_color_hex(on ? accent : ChipColor()), LV_PART_MAIN);
  if (is_label) {
    lv_obj_set_style_text_color(label, lv_color_hex(on ? 0x0B0B0C : ChipText()), LV_PART_MAIN);
  }
}

void RefreshChips(NatureViewState* state) {
  for (size_t i = 0; i < sizeof(kGroupFilters) / sizeof(kGroupFilters[0]); ++i) {
    StyleChip(state->group_chips[i], state->group == static_cast<int>(i));
  }
  for (size_t i = 0; i < sizeof(kUseFilters) / sizeof(kUseFilters[0]); ++i) {
    StyleChip(state->use_chips[i], state->uses & kUseFilters[i].mask, kUseFilters[i].color);
  }
  StyleChip(state->season_chip, state->in_season);
  StyleChip(state->sort_chip, state->by_count);
  if (state->sort_chip != nullptr) {
    lv_label_set_text(lv_obj_get_child(state->sort_chip, 0),
        state->by_count ? "Most seen" : "A-Z");
  }
  for (int i = 0; i < survival::kNatureColorCount; ++i) {
    lv_obj_t* chip = state->color_chips[i];
    if (chip != nullptr) {
      lv_obj_set_style_border_width(chip, state->color == i ? 4 : 0, LV_PART_MAIN);
    }
  }
  for (size_t i = 0; i < sizeof(kHabitats) / sizeof(kHabitats[0]); ++i) {
    StyleChip(state->habitat_chips[i], state->habitats & kHabitats[i].mask);
  }
}

void ChipClickedCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  lv_obj_t* chip = lv_event_get_current_target_obj(event);
  const int tag = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(chip)));
  const int kind = tag / 100;
  const int value = tag % 100;
  switch (kind) {
    case 1: state->group = value; break;
    case 2: state->uses ^= kUseFilters[value].mask; break;
    case 3: state->in_season = !state->in_season; break;
    case 4: state->by_count = !state->by_count; break;
    case 5: state->color = state->color == value ? -1 : value; break;
    case 6: state->habitats ^= kHabitats[value].mask; break;
    default: return;
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  RefreshChips(state);
  ApplyFilters(state);
}

lv_obj_t* AddChipRow(NatureViewState* state, int y) {
  lv_obj_t* row = lv_obj_create(state->root);
  MakePlain(row);
  lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(row, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_size(row, state->width, kChipRowHeight);
  lv_obj_set_pos(row, 0, y);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(row, kSidePadding, LV_PART_MAIN);
  return row;
}

lv_obj_t* AddChip(NatureViewState* state, lv_obj_t* row, const char* text, int tag) {
  lv_obj_t* chip = lv_obj_create(row);
  MakePlain(chip);
  lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(chip, LV_SIZE_CONTENT, kChipRowHeight - 8);
  lv_obj_set_style_pad_hor(chip, 18, LV_PART_MAIN);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_user_data(chip, reinterpret_cast<void*>(static_cast<intptr_t>(tag)));
  lv_obj_add_event_cb(chip, ChipClickedCallback, LV_EVENT_CLICKED, state);
  lv_obj_t* label = CreateLabel(chip, text, ChipText(), &lvgl_font_google_sans_flex_22);
  lv_obj_center(label);
  StyleChip(chip, false);
  return chip;
}

lv_obj_t* AddColorChip(NatureViewState* state, lv_obj_t* row, int index) {
  lv_obj_t* chip = lv_obj_create(row);
  MakePlain(chip);
  lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(chip, kChipRowHeight - 8, kChipRowHeight - 8);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(chip, lv_color_hex(kColors[index].rgb), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_color(chip, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
  lv_obj_set_user_data(chip, reinterpret_cast<void*>(static_cast<intptr_t>(500 + index)));
  lv_obj_add_event_cb(chip, ChipClickedCallback, LV_EVENT_CLICKED, state);
  return chip;
}

void SearchChangedCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  const char* text = lv_textarea_get_text(state->search_field);
  std::snprintf(state->query, sizeof(state->query), "%s", text != nullptr ? text : "");
  ApplyFilters(state);
}

// ── Loading ──────────────────────────────────────────────────────────────────

bool FindPack(NatureViewState* state, char* path, size_t size) {
  if (state->storage == nullptr ||
      (!state->storage->IsSdCardMounted() && !state->storage->EnsureSdCardMounted())) {
    return false;
  }
  char directory[96];
  std::snprintf(directory, sizeof(directory), "%s/%s", state->storage->SdCardBasePath(),
      kNatureDirectory);
  DIR* folder = opendir(directory);
  if (folder == nullptr) {
    return false;
  }
  bool found = false;
  for (dirent* entry = readdir(folder); entry != nullptr; entry = readdir(folder)) {
    const size_t length = std::strlen(entry->d_name);
    if (entry->d_name[0] != '.' && length > 6 &&
        strcasecmp(entry->d_name + length - 6, ".svnat") == 0) {
      std::snprintf(path, size, "%s/%s", directory, entry->d_name);
      found = true;
      break;
    }
  }
  closedir(folder);
  return found;
}

void LoadPack(void* context) {
  auto* state = static_cast<NatureViewState*>(context);
  char path[160] = {};
  if (!FindPack(state, path, sizeof(path)) || !state->pack.Open(path)) {
    lv_label_set_text(state->empty_label,
        "No field guide on the SD card yet.\n\nDownload nature.svnat on the "
        "SurvivalOS web page (Fill the card) and copy it anywhere on the card. "
        "It moves to /nature by itself.");
    lv_obj_remove_flag(state->empty_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(state->count_label, "");
    return;
  }
  const uint32_t count = state->pack.SpeciesCount();
  state->order_by_name = static_cast<uint32_t*>(
      heap_caps_malloc(sizeof(uint32_t) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  state->order_by_count = static_cast<uint32_t*>(
      heap_caps_malloc(sizeof(uint32_t) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  state->filtered = static_cast<uint32_t*>(
      heap_caps_malloc(sizeof(uint32_t) * count, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (state->order_by_name == nullptr || state->order_by_count == nullptr ||
      state->filtered == nullptr) {
    return;
  }
  for (uint32_t i = 0; i < count; ++i) {
    state->order_by_name[i] = i;
    state->order_by_count[i] = i;
  }
  const NaturePack& pack = state->pack;
  std::sort(state->order_by_count, state->order_by_count + count,
      [&pack](uint32_t a, uint32_t b) {
        return pack.Species(a).observations > pack.Species(b).observations;
      });
  state->loaded = true;
  lv_label_set_text(state->empty_label, "Nothing matches these filters.");
  ApplyFilters(state);
}

bool HandleBack(NatureViewState* state) {
  if (state->detail != nullptr) {
    CloseDetail(state);
    return true;
  }
  return false;
}

void ViewDeletedCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  lv_async_call_cancel(LoadPack, state);
  if (state->thumb_timer != nullptr) {
    lv_timer_delete(state->thumb_timer);
  }
  for (Row& row : state->rows) {
    survival::FreeJpegImage(&row.image);
  }
  for (int i = 0; i < state->photo_count; ++i) {
    survival::FreeJpegImage(&state->photos[i]);
  }
  heap_caps_free(state->order_by_name);
  heap_caps_free(state->order_by_count);
  heap_caps_free(state->filtered);
  state->pack.Close();
  delete state;
}

}  // namespace

lv_obj_t* CreateNatureView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config) {
  (void)app_entry;
  if (parent == nullptr || config.width <= 0 || config.height <= 0) {
    return nullptr;
  }
  lv_obj_t* container = lv_obj_create(parent);
  if (container == nullptr) {
    return nullptr;
  }
  MakePlain(container);
  lv_obj_set_size(container, config.width, config.height);
  lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(container, lv_color_hex(Surface()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container, LV_OPA_COVER, LV_PART_MAIN);

  auto* state = new (std::nothrow) NatureViewState();
  if (state == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  state->root = container;
  state->storage = config.storage;
  state->rtc = config.rtc;
  state->width = config.width;
  state->height = config.height;
  lv_obj_add_event_cb(container, ViewDeletedCallback, LV_EVENT_DELETE, state);

  hal::RtcStatus rtc_status;
  if (state->rtc != nullptr && state->rtc->ReadRtcStatus(&rtc_status) && rtc_status.ready &&
      rtc_status.year >= 2024 && rtc_status.month >= 1 && rtc_status.month <= 12) {
    state->current_month = rtc_status.month - 1;
  }

  lv_obj_t* title = CreateLabel(container, "Nature", OnSurface(),
      &lvgl_font_google_sans_flex_36);
  lv_obj_set_pos(title, kSidePadding, kTitleTop);
  state->count_label = CreateLabel(container, "Loading...", Muted(),
      &lvgl_font_google_sans_flex_22);
  lv_obj_align(state->count_label, LV_ALIGN_TOP_RIGHT, -kSidePadding, kTitleTop + 12);

  state->search_field = lv_textarea_create(container);
  lv_textarea_set_one_line(state->search_field, true);
  lv_textarea_set_placeholder_text(state->search_field, "Search Czech, Latin or English names");
  lv_obj_set_size(state->search_field, config.width - 2 * kSidePadding, kSearchHeight);
  lv_obj_set_pos(state->search_field, kSidePadding, kSearchTop);
  lv_obj_set_style_radius(state->search_field, 20, LV_PART_MAIN);
  lv_obj_set_style_text_font(state->search_field, &lvgl_font_google_sans_flex_24, LV_PART_MAIN);
  lv_obj_add_event_cb(state->search_field, SearchChangedCallback, LV_EVENT_VALUE_CHANGED, state);

  int y = kSearchTop + kSearchHeight + 12;
  lv_obj_t* row = AddChipRow(state, y);
  for (size_t i = 0; i < sizeof(kGroupFilters) / sizeof(kGroupFilters[0]); ++i) {
    state->group_chips[i] = AddChip(state, row, kGroupFilters[i].label, 100 + static_cast<int>(i));
  }
  y += kChipRowHeight + kChipGap / 2;
  row = AddChipRow(state, y);
  for (size_t i = 0; i < sizeof(kUseFilters) / sizeof(kUseFilters[0]); ++i) {
    state->use_chips[i] = AddChip(state, row, kUseFilters[i].label, 200 + static_cast<int>(i));
  }
  if (state->current_month >= 0) {
    state->season_chip = AddChip(state, row, "In season now", 300);
  }
  state->sort_chip = AddChip(state, row, "A-Z", 400);
  y += kChipRowHeight + kChipGap / 2;
  row = AddChipRow(state, y);
  for (int i = 0; i < survival::kNatureColorCount; ++i) {
    state->color_chips[i] = AddColorChip(state, row, i);
  }
  for (size_t i = 0; i < sizeof(kHabitats) / sizeof(kHabitats[0]); ++i) {
    state->habitat_chips[i] = AddChip(state, row, kHabitats[i].label, 600 + static_cast<int>(i));
  }
  y += kChipRowHeight + kChipGap;
  RefreshChips(state);

  state->list = lv_obj_create(container);
  MakePlain(state->list);
  lv_obj_add_flag(state->list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(state->list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->list, LV_SCROLLBAR_MODE_ACTIVE);
  lv_obj_set_size(state->list, config.width, config.height - y);
  lv_obj_set_pos(state->list, 0, y);
  lv_obj_add_event_cb(state->list, ListScrollCallback, LV_EVENT_SCROLL, state);
  state->spacer = lv_obj_create(state->list);
  MakePlain(state->spacer);
  lv_obj_remove_flag(state->spacer, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(state->spacer, 1, 1);
  lv_obj_set_pos(state->spacer, 0, 0);
  const int text_left = kSidePadding + kThumbSize + 20;
  const int text_width = config.width - text_left - kSidePadding;
  for (Row& item : state->rows) {
    item.box = lv_obj_create(state->list);
    MakePlain(item.box);
    lv_obj_add_flag(item.box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(item.box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_size(item.box, config.width, kRowHeight);
    lv_obj_set_style_bg_color(item.box, lv_color_hex(0xFFFFFF), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(item.box, 18, LV_STATE_PRESSED);
    lv_obj_add_event_cb(item.box, RowClickedCallback, LV_EVENT_CLICKED, state);
    lv_obj_t* frame = lv_obj_create(item.box);
    MakePlain(frame);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(frame, kThumbSize, kThumbSize);
    lv_obj_set_pos(frame, kSidePadding, (kRowHeight - kThumbSize) / 2);
    lv_obj_set_style_radius(frame, 18, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(frame, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(frame, lv_color_hex(Placeholder()), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, LV_PART_MAIN);
    item.thumb = lv_image_create(frame);
    lv_obj_set_size(item.thumb, kThumbSize, kThumbSize);
    lv_obj_center(item.thumb);
    item.name = CreateLabel(item.box, "", OnSurface(), &lvgl_font_google_sans_flex_28);
    lv_obj_set_width(item.name, text_width);
    lv_label_set_long_mode(item.name, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(item.name, text_left, 18);
    item.latin = CreateLabel(item.box, "", Muted(), &lvgl_font_google_sans_flex_22);
    lv_obj_set_width(item.latin, text_width);
    lv_label_set_long_mode(item.latin, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(item.latin, text_left, 58);
    item.info = CreateLabel(item.box, "", Muted(), &lvgl_font_google_sans_flex_22);
    lv_obj_set_width(item.info, text_width);
    lv_label_set_long_mode(item.info, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(item.info, text_left, 90);
  }
  state->empty_label = CreateLabel(container, "", Muted(), &lvgl_font_google_sans_flex_24);
  lv_obj_set_width(state->empty_label, config.width - 4 * kSidePadding);
  lv_label_set_long_mode(state->empty_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(state->empty_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(state->empty_label, LV_ALIGN_TOP_MID, 0, y + 80);
  lv_obj_add_flag(state->empty_label, LV_OBJ_FLAG_HIDDEN);

  SharedKeyboardConfig keyboard_config;
  keyboard_config.width = config.width;
  keyboard_config.height = config.height / 3;
  state->keyboard = CreateSharedKeyboard(container, keyboard_config);
  if (state->keyboard != nullptr) {
    AttachSharedKeyboardToTextArea(state->keyboard, state->search_field, nullptr);
  }

  RegisterConditionalBackNavigationHandler(container, [state]() { return HandleBack(state); });
  state->thumb_timer = lv_timer_create(ThumbTimerCallback, kThumbTickMs, state);
  // Loaded after the first frame, so the app opens at once even though the
  // tables take a moment to read from the card.
  lv_async_call(LoadPack, state);
  return container;
}

}  // namespace lilygo_box::ui
