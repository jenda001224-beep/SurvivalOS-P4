/*
 * @Description: Field guide of the plants and fungi of the whole world
 * @License: GPL 3.0
 *
 * Reads nature-world.svnat3 (tools/prepare_world_nature.py) from /nature on
 * the SD card. Hundreds of thousands of species stay on the card: only 8 bytes
 * of filter attributes per species are in memory, and each row reads its
 * record when it comes on screen. The list is virtual: a dozen row widgets are
 * moved and refilled while scrolling. Thumbnails are decoded one per timer
 * tick, and only while the list is not moving, so scrolling never waits for
 * the JPEG decoder. A species page shows up to five photos; tapping one opens
 * it full screen, where photos are swiped and a double tap zooms in.
 */
#include "ui/views/nature_view.h"

#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "base/logger.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "firstaid/firstaid_content.h"
#include "hal/providers/rtc_provider.h"
#include "hal/providers/storage_provider.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "survival/jpeg_image.h"
#include "survival/nature_pack.h"
#include "survival/nature_world.h"
#include "ui/haptic_feedback.h"
#include "ui/input/back_navigation_controller.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/survival_palette.h"
#include "ui/theme/theme_provider.h"
#include "ui/widgets/shared_keyboard.h"

namespace lilygo_box::ui {
namespace {

using survival::NatureAttributes;
using survival::NatureRecord;
using survival::NatureWorld;

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
constexpr int kMaxPhotos = 5;
constexpr uint32_t kThumbTickMs = 40;
constexpr uint32_t kSearchDelayMs = 250;
constexpr int64_t kScrollQuietUs = 160 * 1000;
constexpr int kMaxQuery = 48;
constexpr int kMaxHits = 4000;
constexpr int kSwipeDistance = 60;  // finger travel that turns a photo
constexpr int kStatusBarHeight = 50;
constexpr int kZoom = 3;  // double tap zooms to this many times the fitted size

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
uint32_t Danger() { return Pick(0xFF453A, 0xFF453A); }

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
    {"Poisonous", survival::kNaturePoisonous | survival::kNatureDeadly, 0xFF453A},
};

struct HabitatFilter {
  const char* label;
  uint8_t mask;
};
constexpr HabitatFilter kHabitats[] = {
    {"Forest", survival::kNatureForest}, {"Meadow", survival::kNatureMeadow},
    {"Water", survival::kNatureWater}, {"Rocks", survival::kNatureRocks},
    {"Fields", survival::kNatureFields}, {"Gardens", survival::kNatureGardens},
    {"On wood", survival::kNatureWood}, {"Mountains", survival::kNatureMountains},
};

struct RegionFilter {
  const char* label;
  uint8_t mask;
};
constexpr RegionFilter kRegions[] = {
    {"Czechia", survival::kRegionCzechia},
    // Czechia is in Europe too; the packer sets only the first region that matches.
    {"Europe", survival::kRegionEurope | survival::kRegionCzechia},
    {"Asia", survival::kRegionAsia}, {"Africa", survival::kRegionAfrica},
    {"North America", survival::kRegionNorthAmerica},
    {"South America", survival::kRegionSouthAmerica},
    {"Oceania", survival::kRegionOceania},
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
  uint32_t thumbnail = UINT32_MAX;
  survival::JpegImage image;
  lv_image_dsc_t descriptor = {};
  bool has_image = false;
};

struct Viewer {
  lv_obj_t* root = nullptr;
  lv_obj_t* strip = nullptr;
  lv_obj_t* pages[kMaxPhotos] = {};
  lv_obj_t* images[kMaxPhotos] = {};
  lv_obj_t* counter = nullptr;
  lv_obj_t* credit = nullptr;
  bool zoomed[kMaxPhotos] = {};
  int index = 0;
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
  lv_obj_t* region_chips[sizeof(kRegions) / sizeof(kRegions[0])] = {};
  lv_obj_t* color_chips[survival::kNatureColorCount] = {};
  lv_obj_t* habitat_chips[sizeof(kHabitats) / sizeof(kHabitats[0])] = {};
  Row rows[kRowPool];
  lv_timer_t* thumb_timer = nullptr;
  lv_timer_t* search_timer = nullptr;
  int64_t last_scroll_us = 0;

  NatureWorld world;
  bool loaded = false;
  uint32_t* filtered = nullptr;
  uint32_t filtered_count = 0;
  uint32_t* hits = nullptr;     // search results, most observed first

  int group = 0;
  uint8_t uses = 0;
  int color = -1;
  uint8_t habitats = 0;
  uint8_t regions = 0;
  bool in_season = false;
  bool by_name = false;
  char query[kMaxQuery + 1] = {};
  int current_month = -1;

  // Detail page.
  lv_obj_t* detail = nullptr;
  NatureRecord record = {};
  survival::JpegImage photos[kMaxPhotos];
  lv_image_dsc_t photo_descriptors[kMaxPhotos] = {};
  bool photo_ok[kMaxPhotos] = {};
  int photo_count = 0;
  lv_obj_t* photo_credit = nullptr;
  lv_obj_t* photo_dots[kMaxPhotos] = {};
  lv_obj_t* photo_strip = nullptr;
  int photo_index = 0;
  lv_point_t press_point = {};
  Viewer viewer;
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

// "152 340": thousands apart, as Czech writes them.
void FormatCount(uint32_t value, char* out, size_t size) {
  char digits[16];
  const int length = std::snprintf(digits, sizeof(digits), "%lu", static_cast<unsigned long>(value));
  size_t used = 0;
  for (int i = 0; i < length && used + 2 < size; ++i) {
    if (i > 0 && (length - i) % 3 == 0) {
      out[used++] = ' ';
    }
    out[used++] = digits[i];
  }
  out[used] = '\0';
}

// The months as one run ("May–Oct", "Nov–Feb"), or "" when they are scattered.
void MonthRange(uint16_t months, char* out, size_t size) {
  out[0] = '\0';
  months &= 0x0FFF;
  if (months == 0) {
    return;
  }
  if (months == 0x0FFF) {
    std::snprintf(out, size, "All year");
    return;
  }
  int start = -1, runs = 0;
  for (int month = 0; month < 12; ++month) {
    const bool on = months & (1u << month);
    const bool before = months & (1u << ((month + 11) % 12));
    if (on && !before) {
      start = month;
      ++runs;
    }
  }
  if (runs != 1) {
    return;
  }
  int end = start;
  while (months & (1u << ((end + 1) % 12))) {
    end = (end + 1) % 12;
  }
  if (start == end) {
    std::snprintf(out, size, "%s", kMonthNames[start]);
  } else {
    std::snprintf(out, size, "%s–%s", kMonthNames[start], kMonthNames[end]);
  }
}

// Short "Edible · Forest · May–Oct" line for a list row.
void DescribeShort(const NatureAttributes& species, char* out, size_t size, uint32_t* color) {
  size_t used = 0;
  out[0] = '\0';
  *color = Muted();
  auto add = [&](const char* text) {
    if (text[0] == '\0') {
      return;
    }
    if (used > 0 && used + 4 < size) {
      used += std::snprintf(out + used, size - used, " · ");
    }
    if (used < size) {
      used += std::snprintf(out + used, size - used, "%s", text);
    }
  };
  if (species.uses & survival::kNatureDeadly) {
    add("Deadly"); *color = Danger();
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
  char months[24];
  MonthRange(species.months, months, sizeof(months));
  add(months);
}

// ── Filtering ────────────────────────────────────────────────────────────────

bool Matches(const NatureViewState* state, const NatureAttributes& species) {
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
  if (state->regions != 0 && (species.regions & state->regions) == 0) {
    return false;
  }
  if (state->in_season && state->current_month >= 0 &&
      !(species.months & (1u << state->current_month))) {
    return false;
  }
  return true;
}

void LayoutList(NatureViewState* state, bool force);

void ApplyFilters(NatureViewState* state) {
  if (!state->loaded) {
    return;
  }
  NatureWorld& world = state->world;
  const uint32_t total = world.SpeciesCount();
  char folded[kMaxQuery * 2 + 1] = {};
  const size_t query_length = survival::FoldNatureName(state->query, folded, sizeof(folded));
  const uint32_t* source = nullptr;
  uint32_t source_count = total;
  bool capped = false;
  if (query_length >= 2) {
    const int hits = world.Search(state->query, state->hits, kMaxHits);
    source = state->hits;
    source_count = static_cast<uint32_t>(hits);
    capped = hits >= kMaxHits;
  } else if (state->by_name) {
    source = world.OrderByName();
  }
  uint32_t count = 0;
  for (uint32_t i = 0; i < source_count; ++i) {
    const uint32_t species = source != nullptr ? source[i] : i;
    if (species < total && Matches(state, world.Attributes(species))) {
      state->filtered[count++] = species;
    }
  }
  state->filtered_count = count;
  char shown[16], all[16], text[64];
  FormatCount(count, shown, sizeof(shown));
  FormatCount(total, all, sizeof(all));
  std::snprintf(text, sizeof(text), capped ? "%s+ of %s" : "%s of %s", shown, all);
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

// The name to show: Czech, else English, else Latin.
const char* DisplayName(NatureWorld& world, const NatureRecord& record, char* out, size_t size) {
  world.String(record.name_cs, out, size);
  if (out[0] == '\0') world.String(record.name_en, out, size);
  if (out[0] == '\0') world.String(record.name_lat, out, size);
  // Common names come as the community wrote them: "kopřiva dvoudomá".
  auto* first = reinterpret_cast<uint8_t*>(out);
  if (first[0] >= 'a' && first[0] <= 'z') {
    first[0] -= 'a' - 'A';
  } else if (first[0] == 0xC3 && first[1] >= 0xA0 && first[1] <= 0xBE && first[1] != 0xB7) {
    first[1] -= 0x20;  // á é í ó ú ý
  } else if (first[0] == 0xC4 || first[0] == 0xC5) {
    // Latin Extended-A: mostly pairs (even upper, odd lower), č ď ě ň ř š ť ů;
    // in 0x139-0x148 and 0x179-0x17E the pairs are the other way round: ž ľ ł.
    const uint32_t code = ((first[0] & 0x1Fu) << 6) | (first[1] & 0x3Fu);
    const bool odd_is_upper = (code >= 0x139 && code <= 0x148) || (code >= 0x179 && code <= 0x17E);
    if ((code & 1u) == (odd_is_upper ? 0u : 1u)) {
      first[1] -= 1;
    }
  }
  return out;
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
  if (row->species == species_index) {
    return;  // same species, only moved
  }
  ReleaseRowImage(row);
  row->species = species_index;
  NatureWorld& world = state->world;
  NatureRecord record = {};
  if (!world.Record(species_index, &record)) {
    row->thumbnail = UINT32_MAX;
    lv_label_set_text(row->name, "?");
    lv_label_set_text(row->latin, "");
    lv_label_set_text(row->info, "");
    return;
  }
  row->thumbnail = record.thumbnail;
  char name[160], latin[160], family[96];
  DisplayName(world, record, name, sizeof(name));
  lv_label_set_text(row->name, name);
  world.String(record.name_lat, latin, sizeof(latin));
  if (std::strcmp(name, latin) == 0) {
    // No common name: the family says more than the Latin name twice.
    world.String(record.family, family, sizeof(family));
    lv_label_set_text(row->latin, family);
  } else {
    lv_label_set_text(row->latin, latin);
  }
  char info[96] = {};
  uint32_t color = Muted();
  DescribeShort(world.Attributes(species_index), info, sizeof(info), &color);
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
      row->species = UINT32_MAX;
      ReleaseRowImage(row);
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
    size_t size = 0;
    uint8_t* jpeg = row.thumbnail != UINT32_MAX
        ? state->world.ReadPhoto(row.thumbnail, &size) : nullptr;
    if (jpeg != nullptr && survival::DecodeJpegBuffer(jpeg, size, kThumbSize * 3,
                               kThumbSize * 3, &row.image)) {
      FillDescriptor(&row.descriptor, row.image);
      lv_image_set_src(row.thumb, &row.descriptor);
      // Cover the square the way a photo app crops a thumbnail.
      const uint32_t shorter = std::min(row.image.width, row.image.height);
      lv_image_set_scale(row.thumb, shorter > 0 ? 256 * kThumbSize / shorter : 256);
      lv_image_set_inner_align(row.thumb, LV_IMAGE_ALIGN_CENTER);
    }
    row.has_image = true;  // a broken photo is not retried every tick
    heap_caps_free(jpeg);
    return;
  }
}

// ── Full-screen photos ───────────────────────────────────────────────────────

void FitSize(const NatureViewState* state, int index, int* width, int* height) {
  const survival::JpegImage& photo = state->photos[index];
  const int w = std::max<int>(1, photo.width);
  const int h = std::max<int>(1, photo.height);
  if (static_cast<int64_t>(state->width) * h <= static_cast<int64_t>(state->height) * w) {
    *width = state->width;
    *height = state->width * h / w;
  } else {
    *height = state->height;
    *width = state->height * w / h;
  }
}

void PlaceViewerImage(NatureViewState* state, int index, int image_width, int image_height) {
  lv_obj_t* image = state->viewer.images[index];
  lv_obj_set_size(image, image_width, image_height);
  // Smaller than the screen: in the middle. Larger: the page scrolls over it.
  lv_obj_set_pos(image, std::max(0, (state->width - image_width) / 2),
      std::max(0, (state->height - image_height) / 2));
}

void UpdateViewerLabels(NatureViewState* state) {
  Viewer& viewer = state->viewer;
  char text[16];
  std::snprintf(text, sizeof(text), "%d / %d", viewer.index + 1, state->photo_count);
  lv_label_set_text(viewer.counter, text);
  survival::NaturePhotoEntry entry;
  char credit[200] = {};
  if (state->world.Photo(state->record.first_photo + viewer.index, &entry)) {
    state->world.String(entry.credit, credit, sizeof(credit));
  }
  lv_label_set_text(viewer.credit, credit);
}

void SetZoom(NatureViewState* state, int index, bool zoomed, const lv_point_t* at) {
  Viewer& viewer = state->viewer;
  lv_obj_t* page = viewer.pages[index];
  if (page == nullptr || viewer.images[index] == nullptr || viewer.zoomed[index] == zoomed) {
    return;
  }
  int fit_width = 0, fit_height = 0;
  FitSize(state, index, &fit_width, &fit_height);
  viewer.zoomed[index] = zoomed;
  if (!zoomed) {
    lv_obj_scroll_to(page, 0, 0, LV_ANIM_OFF);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    PlaceViewerImage(state, index, fit_width, fit_height);
    return;
  }
  // Keep the tapped point under the finger.
  const int old_x = std::max(0, (state->width - fit_width) / 2);
  const int old_y = std::max(0, (state->height - fit_height) / 2);
  const int new_width = fit_width * kZoom;
  const int new_height = fit_height * kZoom;
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  PlaceViewerImage(state, index, new_width, new_height);
  lv_obj_update_layout(page);
  const int tap_x = at != nullptr ? at->x : state->width / 2;
  const int tap_y = at != nullptr ? at->y : state->height / 2;
  const int image_x = std::clamp(tap_x - old_x, 0, fit_width) * kZoom;
  const int image_y = std::clamp(tap_y - old_y, 0, fit_height) * kZoom;
  const int new_x = std::max(0, (state->width - new_width) / 2);
  const int new_y = std::max(0, (state->height - new_height) / 2);
  const int scroll_x = std::clamp(new_x + image_x - tap_x, 0, std::max(0, new_width - state->width));
  const int scroll_y = std::clamp(new_y + image_y - tap_y, 0,
      std::max(0, new_height - state->height));
  lv_obj_scroll_to(page, scroll_x, scroll_y, LV_ANIM_OFF);
}

void CloseViewer(NatureViewState* state) {
  Viewer& viewer = state->viewer;
  if (viewer.root == nullptr) {
    return;
  }
  lv_obj_delete(viewer.root);
  viewer = Viewer();
}

void ViewerCloseClicked(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  PlayUiHapticEffect(HapticEffect::kSelection);
  CloseViewer(state);
}

void ViewerPageDoubleClicked(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  lv_obj_t* page = lv_event_get_current_target_obj(event);
  const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(page)));
  if (index < 0 || index >= kMaxPhotos) {
    return;
  }
  lv_point_t point = {};
  lv_indev_t* indev = lv_indev_active();
  if (indev != nullptr) {
    lv_indev_get_point(indev, &point);
    lv_area_t area;
    lv_obj_get_coords(page, &area);
    point.x -= area.x1;
    point.y -= area.y1;
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  SetZoom(state, index, !state->viewer.zoomed[index], indev != nullptr ? &point : nullptr);
}

lv_point_t ActivePoint() {
  lv_point_t point = {};
  lv_indev_t* indev = lv_indev_active();
  if (indev != nullptr) {
    lv_indev_get_point(indev, &point);
  }
  return point;
}

// One swipe, one photo. LVGL's own snapping of a scrolling strip threw two
// photos at a time; these strips do not scroll by touch at all.
void ViewerPagePressCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  Viewer& viewer = state->viewer;
  const lv_point_t point = ActivePoint();
  if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
    state->press_point = point;
    return;
  }
  if (viewer.zoomed[viewer.index]) {
    return;  // dragging moves the zoomed photo
  }
  const int dx = point.x - state->press_point.x;
  const int dy = point.y - state->press_point.y;
  if (std::abs(dx) < kSwipeDistance || std::abs(dx) < std::abs(dy)) {
    return;
  }
  const int next = viewer.index + (dx < 0 ? 1 : -1);
  if (next < 0 || next >= state->photo_count) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  viewer.index = next;
  lv_obj_scroll_to_x(viewer.strip, next * state->width, LV_ANIM_ON);
  UpdateViewerLabels(state);
}

lv_obj_t* RoundButton(lv_obj_t* parent, const char* glyph, lv_event_cb_t callback,
    NatureViewState* state) {
  lv_obj_t* button = lv_button_create(parent);
  lv_obj_add_flag(button, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(button, 76, 76);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(button, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, state);
  lv_obj_t* label = CreateLabel(button, glyph, Primary(), &lvgl_font_material_symbols_fill_32);
  lv_obj_center(label);
  return button;
}

void OpenViewer(NatureViewState* state, int index) {
  CloseViewer(state);
  if (state->photo_count == 0) {
    return;
  }
  Viewer& viewer = state->viewer;
  viewer.index = std::clamp(index, 0, state->photo_count - 1);
  viewer.root = lv_obj_create(state->root);
  MakePlain(viewer.root);
  lv_obj_set_size(viewer.root, state->width, state->height);
  lv_obj_set_pos(viewer.root, 0, 0);
  // Photos are the point here, so black even outside Survival Mode.
  lv_obj_set_style_bg_color(viewer.root, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(viewer.root, LV_OPA_COVER, LV_PART_MAIN);

  viewer.strip = lv_obj_create(viewer.root);
  MakePlain(viewer.strip);
  lv_obj_set_size(viewer.strip, state->width, state->height);
  lv_obj_set_scroll_dir(viewer.strip, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(viewer.strip, LV_SCROLLBAR_MODE_OFF);
  for (int i = 0; i < state->photo_count; ++i) {
    lv_obj_t* page = lv_obj_create(viewer.strip);
    MakePlain(page);
    viewer.pages[i] = page;
    lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(page, ViewerPagePressCallback, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(page, ViewerPagePressCallback, LV_EVENT_RELEASED, state);
    lv_obj_set_scroll_dir(page, LV_DIR_ALL);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(page, state->width, state->height);
    lv_obj_set_pos(page, i * state->width, 0);
    lv_obj_set_user_data(page, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(page, ViewerPageDoubleClicked, LV_EVENT_DOUBLE_CLICKED, state);
    if (!state->photo_ok[i]) {
      lv_obj_t* missing = CreateLabel(page, "Photo could not be read", Muted(),
          &lvgl_font_google_sans_flex_24);
      lv_obj_center(missing);
      continue;
    }
    lv_obj_t* image = lv_image_create(page);
    viewer.images[i] = image;
    lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    lv_image_set_src(image, &state->photo_descriptors[i]);
    lv_image_set_inner_align(image, LV_IMAGE_ALIGN_STRETCH);
    int fit_width = 0, fit_height = 0;
    FitSize(state, i, &fit_width, &fit_height);
    PlaceViewerImage(state, i, fit_width, fit_height);
  }

  lv_obj_t* close = RoundButton(viewer.root, icon::kClose, ViewerCloseClicked, state);
  lv_obj_set_pos(close, 18, 60);
  viewer.counter = CreateLabel(viewer.root, "", 0xFFFFFF, &lvgl_font_google_sans_flex_24);
  lv_obj_set_style_bg_color(viewer.counter, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(viewer.counter, LV_OPA_60, LV_PART_MAIN);
  lv_obj_set_style_radius(viewer.counter, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(viewer.counter, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(viewer.counter, 10, LV_PART_MAIN);
  lv_obj_align(viewer.counter, LV_ALIGN_TOP_RIGHT, -18, 74);
  viewer.credit = CreateLabel(viewer.root, "", 0xD1D1D6, &lvgl_font_google_sans_flex_22);
  lv_obj_set_width(viewer.credit, state->width - 2 * kSidePadding);
  lv_label_set_long_mode(viewer.credit, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_bg_color(viewer.credit, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(viewer.credit, LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_pad_all(viewer.credit, 8, LV_PART_MAIN);
  lv_obj_set_style_radius(viewer.credit, 10, LV_PART_MAIN);
  lv_obj_align(viewer.credit, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_obj_t* hint = CreateLabel(viewer.root, "Double tap to zoom", 0x8E8E93,
      &lvgl_font_google_sans_flex_22);
  lv_obj_align_to(hint, viewer.credit, LV_ALIGN_OUT_TOP_MID, 0, -8);
  lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
  UpdateViewerLabels(state);
  lv_obj_update_layout(viewer.root);
  lv_obj_scroll_to_x(viewer.strip, viewer.index * state->width, LV_ANIM_OFF);
}

// ── Detail page ───────────────────────────────────────────────────────────────

void CloseDetail(NatureViewState* state) {
  CloseViewer(state);
  if (state->detail == nullptr) {
    return;
  }
  lv_obj_delete(state->detail);
  state->detail = nullptr;
  for (int i = 0; i < state->photo_count; ++i) {
    lv_image_cache_drop(&state->photo_descriptors[i]);
    survival::FreeJpegImage(&state->photos[i]);
    state->photo_ok[i] = false;
  }
  state->photo_count = 0;
}

void BackButtonCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  PlayUiHapticEffect(HapticEffect::kSelection);
  CloseDetail(state);
}

void ShowDetailPhoto(NatureViewState* state, int index) {
  state->photo_index = index;
  lv_obj_scroll_to_x(state->photo_strip, index * state->width, LV_ANIM_ON);
  for (int i = 0; i < state->photo_count; ++i) {
    if (state->photo_dots[i] != nullptr) {
      lv_obj_set_style_bg_opa(state->photo_dots[i], i == index ? LV_OPA_COVER : LV_OPA_40,
          LV_PART_MAIN);
    }
  }
  survival::NaturePhotoEntry entry;
  char credit[200] = {};
  if (state->world.Photo(state->record.first_photo + index, &entry)) {
    state->world.String(entry.credit, credit, sizeof(credit));
  }
  lv_label_set_text(state->photo_credit, credit);
}

// A sideways swipe turns one photo; a tap opens it full screen; anything else
// is the page scrolling up and down.
void PhotoFrameCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  const lv_point_t point = ActivePoint();
  if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
    state->press_point = point;
    return;
  }
  const int dx = point.x - state->press_point.x;
  const int dy = point.y - state->press_point.y;
  if (std::abs(dx) >= kSwipeDistance && std::abs(dx) >= std::abs(dy)) {
    const int next = state->photo_index + (dx < 0 ? 1 : -1);
    if (next >= 0 && next < state->photo_count) {
      PlayUiHapticEffect(HapticEffect::kSelection);
      ShowDetailPhoto(state, next);
    }
    return;
  }
  if (std::abs(dx) < 24 && std::abs(dy) < 24) {
    PlayUiHapticEffect(HapticEffect::kImpactLight);
    OpenViewer(state, state->photo_index);
  }
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

lv_obj_t* AddWrappedLabel(lv_obj_t* parent, const char* text, uint32_t color,
    const lv_font_t* font, int width) {
  lv_obj_t* label = CreateLabel(parent, text, color, font);
  lv_obj_set_width(label, width);
  lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  return label;
}

void AddText(lv_obj_t* parent, const char* text, int width) {
  // Plain-text extracts mark sections as "== Title =="; those become headings.
  const char* cursor = text;
  char line[1600];
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
    }
    lv_obj_t* label = AddWrappedLabel(parent, start,
        heading ? OnSurface() : Pick(0xD1D1D6, ui::survival_look::kDim),
        heading ? &lvgl_font_google_sans_flex_28 : &lvgl_font_google_sans_flex_24, width);
    lv_obj_set_style_text_line_space(label, heading ? 0 : 5, LV_PART_MAIN);
    if (heading) {
      lv_obj_set_style_pad_top(label, 14, LV_PART_MAIN);
    }
  }
}

void PoisonCardClicked(lv_event_t* event) {
  lv_obj_t* card = lv_event_get_current_target_obj(event);
  lv_obj_t* steps = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
  if (card == nullptr || steps == nullptr) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  if (lv_obj_has_flag(steps, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_remove_flag(steps, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(steps, LV_OBJ_FLAG_HIDDEN);
  }
}

// What to do when someone ate it: open on poisonous species, a tap away on
// the rest (an unknown plant may still be what made someone ill).
void AddPoisonCard(lv_obj_t* body, const NatureAttributes& species, int width) {
  const bool deadly = species.uses & survival::kNatureDeadly;
  const bool poisonous = deadly || (species.uses & survival::kNaturePoisonous);
  const uint32_t color = poisonous ? Danger() : Muted();
  lv_obj_t* card = lv_obj_create(body);
  MakePlain(card);
  lv_obj_set_size(card, width, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 18, LV_PART_MAIN);
  lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(card, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_border_opa(card, poisonous ? LV_OPA_COVER : LV_OPA_50, LV_PART_MAIN);
  if (!survival_look::Active() && poisonous) {
    lv_obj_set_style_bg_color(card, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, 30, LV_PART_MAIN);
  }
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  const int inner = width - 32;
  const char* title = deadly ? "Smrtelně jedovaté – když to někdo snědl"
      : poisonous ? "Jedovaté – když to někdo snědl"
                  : "Někdo to snědl a je mu zle?";
  AddWrappedLabel(card, title, poisonous ? color : OnSurface(), &lvgl_font_google_sans_flex_28,
      inner);
  lv_obj_t* steps = lv_obj_create(card);
  MakePlain(steps);
  lv_obj_remove_flag(steps, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(steps, inner, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(steps, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(steps, 8, LV_PART_MAIN);
  const char* cursor = firstaid::kPoisoningSteps;
  int number = 1;
  char line[400];
  while (cursor != nullptr && *cursor != '\0') {
    const char* end = std::strchr(cursor, '\n');
    const size_t length = end != nullptr ? static_cast<size_t>(end - cursor) : std::strlen(cursor);
    std::snprintf(line, sizeof(line), "%d.  %.*s", number++, static_cast<int>(length), cursor);
    AddWrappedLabel(steps, line, Pick(0xE5E5EA, survival_look::kText),
        &lvgl_font_google_sans_flex_24, inner);
    cursor = end != nullptr ? end + 1 : nullptr;
  }
  AddWrappedLabel(steps, "Víc v aplikaci First Aid: Otrava rostlinou, Otrava houbou.", Muted(),
      &lvgl_font_google_sans_flex_22, inner);
  if (!poisonous) {
    lv_obj_add_flag(steps, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_add_event_cb(card, PoisonCardClicked, LV_EVENT_CLICKED, steps);
}

void OpenDetail(NatureViewState* state, uint32_t species_index) {
  CloseDetail(state);
  NatureWorld& world = state->world;
  if (!world.Record(species_index, &state->record)) {
    return;
  }
  const NatureRecord& record = state->record;
  const NatureAttributes& species = world.Attributes(species_index);
  const int width = state->width;
  const int content_width = width - 2 * kSidePadding;

  lv_obj_t* page = lv_obj_create(state->root);
  state->detail = page;
  MakePlain(page);
  lv_obj_add_flag(page, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(page, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
  // Below the status bar, so nothing scrolls under the clock.
  lv_obj_set_size(page, width, state->height - kStatusBarHeight);
  lv_obj_set_pos(page, 0, kStatusBarHeight);
  lv_obj_set_style_bg_color(page, lv_color_hex(Surface()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_bottom(page, 60, LV_PART_MAIN);
  lv_obj_set_style_pad_row(page, 14, LV_PART_MAIN);

  // Photos, swiped sideways; a tap opens them full screen.
  const int photo_height = width * kPhotoHeightPercent / 100;
  lv_obj_t* strip = lv_obj_create(page);
  MakePlain(strip);
  lv_obj_set_size(strip, width, photo_height);
  lv_obj_set_scroll_dir(strip, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(strip, LV_SCROLLBAR_MODE_OFF);
  state->photo_strip = strip;
  state->photo_index = 0;
  lv_obj_set_style_bg_color(strip, lv_color_hex(Placeholder()), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, LV_PART_MAIN);
  state->photo_count = std::min<int>(species.photo_count, kMaxPhotos);
  for (int i = 0; i < state->photo_count; ++i) {
    lv_obj_t* frame = lv_obj_create(strip);
    MakePlain(frame);
    lv_obj_add_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(frame, width, photo_height);
    lv_obj_set_pos(frame, i * width, 0);
    lv_obj_set_style_clip_corner(frame, true, LV_PART_MAIN);
    lv_obj_set_user_data(frame, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(frame, PhotoFrameCallback, LV_EVENT_PRESSED, state);
    lv_obj_add_event_cb(frame, PhotoFrameCallback, LV_EVENT_RELEASED, state);
    size_t size = 0;
    uint8_t* jpeg = world.ReadPhoto(record.first_photo + i, &size);
    // Full size (the pack stores 500 px), so the full-screen view is sharp.
    if (jpeg != nullptr && survival::DecodeJpegBuffer(jpeg, size, width * 2, state->height,
                               &state->photos[i])) {
      state->photo_ok[i] = true;
      FillDescriptor(&state->photo_descriptors[i], state->photos[i]);
      lv_obj_t* image = lv_image_create(frame);
      lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
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
  if (state->photo_count > 0) {
    lv_obj_t* expand = CreateLabel(strip, icon::kOpenInFull, 0xFFFFFF,
        &lvgl_font_material_symbols_fill_32);
    lv_obj_add_flag(expand, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(expand, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(expand, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(expand, LV_OPA_50, LV_PART_MAIN);
    lv_obj_set_style_radius(expand, 12, LV_PART_MAIN);
    lv_obj_set_style_pad_all(expand, 8, LV_PART_MAIN);
    lv_obj_align(expand, LV_ALIGN_BOTTOM_RIGHT, -14, -14);
  }

  lv_obj_t* back = RoundButton(page, icon::kArrowBack, BackButtonCallback, state);
  lv_obj_set_pos(back, 18, 10);

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
  for (int i = 0; i < kMaxPhotos; ++i) {
    state->photo_dots[i] = nullptr;
  }
  for (int i = 0; i < state->photo_count; ++i) {
    lv_obj_t* dot = lv_obj_create(dots);
    MakePlain(dot);
    lv_obj_set_size(dot, 10, 10);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, i == 0 ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    state->photo_dots[i] = dot;
  }
  char credit[200] = {};
  survival::NaturePhotoEntry first_photo;
  if (state->photo_count > 0 && world.Photo(record.first_photo, &first_photo)) {
    world.String(first_photo.credit, credit, sizeof(credit));
  }
  state->photo_credit = CreateLabel(under, credit, Muted(), &lvgl_font_google_sans_flex_22);
  lv_obj_set_width(state->photo_credit, content_width);
  lv_label_set_long_mode(state->photo_credit, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(state->photo_credit, 0, 22);

  lv_obj_t* body = lv_obj_create(page);
  MakePlain(body);
  lv_obj_set_size(body, content_width, LV_SIZE_CONTENT);
  lv_obj_set_style_margin_left(body, kSidePadding, LV_PART_MAIN);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 12, LV_PART_MAIN);

  char name[200];
  DisplayName(world, record, name, sizeof(name));
  AddWrappedLabel(body, name, OnSurface(), &lvgl_font_google_sans_flex_36, content_width);
  char latin[160], english[160], family[96], line[480];
  world.String(record.name_lat, latin, sizeof(latin));
  world.String(record.name_en, english, sizeof(english));
  world.String(record.family, family, sizeof(family));
  size_t used = std::snprintf(line, sizeof(line), "%s", latin);
  if (english[0] != '\0' && std::strcmp(english, name) != 0 && used < sizeof(line)) {
    used += std::snprintf(line + used, sizeof(line) - used, " · %s", english);
  }
  if (family[0] != '\0' && used < sizeof(line)) {
    std::snprintf(line + used, sizeof(line) - used, " · %s", family);
  }
  AddWrappedLabel(body, line, Muted(), &lvgl_font_google_sans_flex_24, content_width);

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
    AddWrappedLabel(body,
        "Only eat or use what you can identify with complete certainty. Many edible "
        "species have poisonous look-alikes.",
        0xFFD60A, &lvgl_font_google_sans_flex_22, content_width);
  }
  AddPoisonCard(body, species, content_width);

  // When it is seen: one box per month.
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
  for (const RegionFilter& region : kRegions) {
    // Czechia also counts as Europe; show Europe only when not already Czechia.
    const uint8_t own = region.mask == (survival::kRegionEurope | survival::kRegionCzechia)
        ? survival::kRegionEurope : region.mask;
    if (species.regions & own) {
      AddPill(places, region.label, 0x98989D);
    }
  }
  for (const HabitatFilter& habitat : kHabitats) {
    if (species.habitats & habitat.mask) {
      AddPill(places, habitat.label, 0xC7C7CC);
    }
  }
  if (species.color < survival::kNatureColorCount) {
    AddPill(places, kColors[species.color].name, kColors[species.color].rgb);
  }

  char* text = world.ReadText(record);
  if (text != nullptr) {
    AddText(body, text, content_width);
    heap_caps_free(text);
  }
  char observations[16], seen[96];
  FormatCount(record.observations, observations, sizeof(observations));
  std::snprintf(seen, sizeof(seen), "Seen %s times on iNaturalist", observations);
  AddWrappedLabel(body, seen, Muted(), &lvgl_font_google_sans_flex_22, content_width);
  char text_credit[200];
  world.String(record.text_credit, text_credit, sizeof(text_credit));
  if (text_credit[0] != '\0') {
    AddWrappedLabel(body, text_credit, Muted(), &lvgl_font_google_sans_flex_22, content_width);
  }
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
  StyleChip(state->sort_chip, state->by_name);
  if (state->sort_chip != nullptr) {
    lv_label_set_text(lv_obj_get_child(state->sort_chip, 0),
        state->by_name ? "A-Z" : "Most seen");
  }
  for (size_t i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); ++i) {
    StyleChip(state->region_chips[i], (state->regions & kRegions[i].mask) == kRegions[i].mask);
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

void ToggleRegion(NatureViewState* state, int index) {
  const uint8_t mask = kRegions[index].mask;
  if ((state->regions & mask) == mask) {
    state->regions &= static_cast<uint8_t>(~mask);
    // Turning Europe off leaves Czechia on only when its own chip is on.
  } else {
    state->regions |= mask;
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
    case 4: state->by_name = !state->by_name; break;
    case 5: state->color = state->color == value ? -1 : value; break;
    case 6: state->habitats ^= kHabitats[value].mask; break;
    case 7: ToggleRegion(state, value); break;
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

// The search reads the card, so it waits until typing pauses.
void SearchTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<NatureViewState*>(lv_timer_get_user_data(timer));
  lv_timer_pause(timer);
  if (state != nullptr) {
    ApplyFilters(state);
  }
}

void SearchChangedCallback(lv_event_t* event) {
  auto* state = static_cast<NatureViewState*>(lv_event_get_user_data(event));
  const char* text = lv_textarea_get_text(state->search_field);
  std::snprintf(state->query, sizeof(state->query), "%s", text != nullptr ? text : "");
  if (state->search_timer != nullptr) {
    lv_timer_reset(state->search_timer);
    lv_timer_resume(state->search_timer);
  }
}

// ── Loading ──────────────────────────────────────────────────────────────────

void LoadPack(void* context) {
  auto* state = static_cast<NatureViewState*>(context);
  char directory[96];
  char path[200] = {};
  bool found = false;
  if (state->storage != nullptr &&
      (state->storage->IsSdCardMounted() || state->storage->EnsureSdCardMounted())) {
    std::snprintf(directory, sizeof(directory), "%s/%s", state->storage->SdCardBasePath(),
        kNatureDirectory);
    found = survival::FindNatureWorld(directory, path, sizeof(path));
  }
  if (!found || !state->world.Open(path)) {
    lv_label_set_text(state->empty_label,
        "No field guide on the SD card yet.\n\nOn the SurvivalOS web page choose "
        "\"Plants & fungi of the whole world\" and put the files anywhere on the "
        "card. They move to /nature by themselves.");
    lv_obj_remove_flag(state->empty_label, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(state->count_label, "");
    return;
  }
  const uint32_t count = state->world.SpeciesCount();
  state->filtered = static_cast<uint32_t*>(
      heap_caps_malloc(sizeof(uint32_t) * std::max<uint32_t>(1, count),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  state->hits = static_cast<uint32_t*>(
      heap_caps_malloc(sizeof(uint32_t) * kMaxHits, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (state->filtered == nullptr || state->hits == nullptr) {
    lv_label_set_text(state->count_label, "Out of memory");
    return;
  }
  char total[16], placeholder[96];
  FormatCount(count, total, sizeof(total));
  std::snprintf(placeholder, sizeof(placeholder), "Search %s species", total);
  lv_textarea_set_placeholder_text(state->search_field, placeholder);
  state->loaded = true;
  lv_label_set_text(state->empty_label, "Nothing matches these filters.");
  ApplyFilters(state);
}

bool HandleBack(NatureViewState* state) {
  if (state->viewer.root != nullptr) {
    CloseViewer(state);
    return true;
  }
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
  if (state->search_timer != nullptr) {
    lv_timer_delete(state->search_timer);
  }
  // The widgets go with the container; only the pixels are ours.
  for (Row& row : state->rows) {
    lv_image_cache_drop(&row.descriptor);
    survival::FreeJpegImage(&row.image);
  }
  for (int i = 0; i < state->photo_count; ++i) {
    lv_image_cache_drop(&state->photo_descriptors[i]);
    survival::FreeJpegImage(&state->photos[i]);
  }
  heap_caps_free(state->filtered);
  heap_caps_free(state->hits);
  state->world.Close();
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
  lv_textarea_set_placeholder_text(state->search_field, "Search Czech, English or Latin names");
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
  state->sort_chip = AddChip(state, row, "Most seen", 400);
  y += kChipRowHeight + kChipGap / 2;
  row = AddChipRow(state, y);
  for (size_t i = 0; i < sizeof(kRegions) / sizeof(kRegions[0]); ++i) {
    state->region_chips[i] = AddChip(state, row, kRegions[i].label, 700 + static_cast<int>(i));
  }
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
  state->search_timer = lv_timer_create(SearchTimerCallback, kSearchDelayMs, state);
  lv_timer_pause(state->search_timer);
  // Loaded after the first frame, so the app opens at once even though the
  // tables take a moment to read from the card.
  lv_async_call(LoadPack, state);
  return container;
}

}  // namespace lilygo_box::ui
