/*
 * @Description: SurvivalOS setup assistant shown on the first start
 * @License: GPL 3.0
 *
 * Welcome -> edition (FULL / MINI) -> SD card -> what goes on it -> where to
 * get the files (QR code to the web page) -> Wi-Fi -> done. MINI skips the
 * three card pages. Every step is plain LVGL objects on an opaque black page;
 * nothing fades or slides, because animating a full-screen object makes LVGL
 * repaint all 568x1232 pixels in software on every frame.
 */
#include "ui/views/setup_wizard_view.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>

#include "app/build_stamp.h"
#include "base/logger.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/providers/storage_provider.h"
#include "ui/haptic_feedback.h"
#include "ui/input/press_cancel.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"

namespace lilygo_box::ui {
namespace {

// Same palette as the web page: black for the AMOLED panel, signal orange.
constexpr uint32_t kBackgroundColor = 0x000000;
constexpr uint32_t kCardColor = 0x1C1C1E;
constexpr uint32_t kCardBorderColor = 0x2C2C2E;
constexpr uint32_t kSelectedCardColor = 0x2A1606;
constexpr uint32_t kAccentColor = 0xFF7A1A;
constexpr uint32_t kOnAccentColor = 0x1A0E04;
constexpr uint32_t kTextColor = 0xFFFFFF;
constexpr uint32_t kSecondaryTextColor = 0x98989F;
constexpr uint32_t kGoodColor = 0x30D158;
constexpr uint32_t kDotColor = 0x3A3A3C;

constexpr int kSidePadding = 40;
constexpr int kTopBarY = 52;
constexpr int kBackButtonSize = 76;
constexpr int kTitleTop = 164;
constexpr int kSubtitleGap = 18;
constexpr int kBodyGap = 40;
constexpr int kPrimaryButtonHeight = 92;
constexpr int kSecondaryButtonHeight = 72;
constexpr int kBottomMargin = 56;
constexpr int kButtonGap = 10;
constexpr int kButtonRadius = 26;
constexpr int kCardRadius = 28;
constexpr int kOptionRowHeight = 84;
constexpr int kOptionRowGap = 12;
constexpr int kQrSize = 300;
constexpr uint32_t kStatusRefreshMs = 400;

// Sizes of the downloads the content page offers, in bytes. Wikipedia sizes
// are the current Kiwix files (download.kiwix.org, "nopic" = no pictures).
constexpr uint64_t kMapsCzechiaBytes = 1'703'518'651ULL;
constexpr uint64_t kMapsWorldBytes = 3'700'000'000ULL;
constexpr uint64_t kWikipediaCzechBytes = 3'430'401'584ULL;
constexpr uint64_t kWikipediaEnglishBytes = 52'690'706'555ULL;
constexpr uint64_t kNatureBytes = 15'688'705'932ULL;

constexpr char kContentPageUrl[] =
    "https://jenda001224-beep.github.io/SurvivalOS-P4/";
constexpr char kContentPageShownUrl[] = "jenda001224-beep.github.io/SurvivalOS-P4";

enum class Step : uint8_t {
  kWelcome,
  kEdition,
  kCard,
  kContent,
  kFiles,
  kWifi,
  kDone,
};

// Looks for the SD card from its own task: mounting a card that is not there
// takes about a second, far too long for the LVGL task.
struct CardProbe {
  std::atomic<bool> wanted{false};
  std::atomic<bool> task_running{false};
  std::atomic<bool> mounted{false};
  std::atomic<bool> info_ready{false};
  std::atomic<uint32_t> total_mib{0};
  std::atomic<uint32_t> free_mib{0};
  hal::StorageProvider* storage = nullptr;
};

CardProbe g_card_probe;

void CardProbeTask(void*) {
  while (g_card_probe.wanted.load()) {
    hal::StorageProvider* storage = g_card_probe.storage;
    bool mounted = storage != nullptr && storage->IsSdCardMounted();
    if (!mounted && storage != nullptr) {
      mounted = storage->EnsureSdCardMounted();
    }
    g_card_probe.mounted.store(mounted);
    if (!mounted) {
      g_card_probe.info_ready.store(false);
    } else if (!g_card_probe.info_ready.load()) {
      uint64_t total_bytes = 0;
      uint64_t free_bytes = 0;
      if (esp_vfs_fat_info(storage->SdCardBasePath(), &total_bytes,
              &free_bytes) == ESP_OK) {
        g_card_probe.total_mib.store(
            static_cast<uint32_t>(total_bytes >> 20));
        g_card_probe.free_mib.store(static_cast<uint32_t>(free_bytes >> 20));
        g_card_probe.info_ready.store(true);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(mounted ? 1500 : 2500));
  }
  g_card_probe.task_running.store(false);
  vTaskDelete(nullptr);
}

void StartCardProbe(hal::StorageProvider* storage) {
  if (storage == nullptr) {
    return;
  }
  g_card_probe.storage = storage;
  g_card_probe.wanted.store(true);
  bool expected = false;
  if (!g_card_probe.task_running.compare_exchange_strong(expected, true)) {
    return;
  }
  if (xTaskCreate(CardProbeTask, "setup_sd", 6144, nullptr, 3, nullptr) !=
      pdPASS) {
    g_card_probe.task_running.store(false);
    LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
        "Start SD card probe failed\n");
  }
}

void StopCardProbe() { g_card_probe.wanted.store(false); }

struct WizardState {
  SetupWizardViewOptions options;
  Step step = Step::kWelcome;
  app::Edition edition = app::Edition::kFull;
  app::MapsChoice maps = app::MapsChoice::kCzechia;
  app::WikipediaChoice wikipedia = app::WikipediaChoice::kCzech;
  bool nature = true;
  bool open_wifi_settings = false;
  bool finishing = false;
  // Step to build once the current event has finished; the button that asked
  // for it is deleted along with the old step.
  Step pending_step = Step::kWelcome;
  bool step_change_pending = false;

  lv_obj_t* root = nullptr;
  // Holds the current step; emptied and rebuilt on every step change.
  lv_obj_t* page = nullptr;
  lv_timer_t* status_timer = nullptr;

  // Live parts of the card and content steps.
  lv_obj_t* card_icon_circle = nullptr;
  lv_obj_t* card_title = nullptr;
  lv_obj_t* card_detail = nullptr;
  lv_obj_t* primary_label = nullptr;
  lv_obj_t* map_rows[3] = {};
  lv_obj_t* wikipedia_rows[4] = {};
  lv_obj_t* nature_rows[2] = {};
  lv_obj_t* total_label = nullptr;
  lv_obj_t* edition_cards[2] = {};
  // What the live parts show now, so the timer only touches them on change.
  int shown_card_state = -1;
};

const lv_font_t* Font22() { return &lvgl_font_google_sans_flex_22; }
const lv_font_t* Font24() { return &lvgl_font_google_sans_flex_24; }
const lv_font_t* Font28() { return &lvgl_font_google_sans_flex_28; }
const lv_font_t* Font36() { return &lvgl_font_google_sans_flex_36; }
const lv_font_t* Font48() { return &lvgl_font_google_sans_flex_48; }

int Width(const WizardState* state) { return state->options.screen_width; }
int Height(const WizardState* state) { return state->options.screen_height; }
int ContentWidth(const WizardState* state) {
  return Width(state) - 2 * kSidePadding;
}
int PrimaryButtonTop(const WizardState* state) {
  return Height(state) - kBottomMargin - kPrimaryButtonHeight;
}
int SecondaryButtonTop(const WizardState* state) {
  return PrimaryButtonTop(state) - kButtonGap - kSecondaryButtonHeight;
}

void MakePlain(lv_obj_t* object) {
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(object, 0, LV_PART_MAIN);
}

lv_obj_t* CreateText(lv_obj_t* parent, const char* text, uint32_t color,
    const lv_font_t* font, int width = 0) {
  lv_obj_t* label = lv_label_create(parent);
  if (label == nullptr) {
    return nullptr;
  }
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  if (width > 0) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  }
  return label;
}

lv_obj_t* CreateIconCircle(lv_obj_t* parent, const char* symbol,
    const lv_font_t* font, int size, uint32_t background, uint32_t color) {
  lv_obj_t* circle = lv_obj_create(parent);
  if (circle == nullptr) {
    return nullptr;
  }
  MakePlain(circle);
  lv_obj_remove_flag(circle, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(circle, size, size);
  lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(circle, lv_color_hex(background), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_t* icon = CreateText(circle, symbol, color, font);
  if (icon != nullptr) {
    lv_obj_center(icon);
  }
  return circle;
}

void FormatGigabytes(uint64_t bytes, char* buffer, size_t size) {
  const uint64_t tenths = (bytes + 50'000'000ULL) / 100'000'000ULL;
  std::snprintf(buffer, size, "%" PRIu64 ".%" PRIu64 " GB", tenths / 10,
      tenths % 10);
}

uint64_t MapsBytes(app::MapsChoice maps) {
  switch (maps) {
    case app::MapsChoice::kCzechia:
      return kMapsCzechiaBytes;
    case app::MapsChoice::kWorld:
      return kMapsWorldBytes;
    case app::MapsChoice::kNone:
      break;
  }
  return 0;
}

uint64_t WikipediaBytes(app::WikipediaChoice wikipedia) {
  switch (wikipedia) {
    case app::WikipediaChoice::kCzech:
      return kWikipediaCzechBytes;
    case app::WikipediaChoice::kEnglish:
      return kWikipediaEnglishBytes;
    case app::WikipediaChoice::kBoth:
      return kWikipediaCzechBytes + kWikipediaEnglishBytes;
    case app::WikipediaChoice::kNone:
      break;
  }
  return 0;
}

bool HasContent(const WizardState* state) {
  return state->maps != app::MapsChoice::kNone ||
      state->wikipedia != app::WikipediaChoice::kNone || state->nature;
}

// The order of the steps for the chosen edition.
size_t StepSequence(const WizardState* state, Step* steps, size_t capacity) {
  size_t count = 0;
  auto add = [&](Step step) {
    if (count < capacity) {
      steps[count++] = step;
    }
  };
  add(Step::kWelcome);
  add(Step::kEdition);
  if (state->edition == app::Edition::kFull) {
    add(Step::kCard);
    add(Step::kContent);
    if (HasContent(state)) {
      add(Step::kFiles);
    }
  }
  add(Step::kWifi);
  add(Step::kDone);
  return count;
}

void ShowStep(WizardState* state, Step step);

void ApplyPendingStep(void* user_data) {
  auto* state = static_cast<WizardState*>(user_data);
  if (state == nullptr || !state->step_change_pending) {
    return;
  }
  state->step_change_pending = false;
  ShowStep(state, state->pending_step);
}

void GoRelative(WizardState* state, int delta) {
  Step steps[8];
  const size_t count = StepSequence(state, steps, 8);
  size_t index = 0;
  for (size_t i = 0; i < count; ++i) {
    if (steps[i] == state->step) {
      index = i;
      break;
    }
  }
  const int next = std::clamp(static_cast<int>(index) + delta, 0,
      static_cast<int>(count) - 1);
  state->pending_step = steps[next];
  if (!state->step_change_pending) {
    state->step_change_pending = true;
    if (lv_async_call(ApplyPendingStep, state) != LV_RESULT_OK) {
      state->step_change_pending = false;
    }
  }
}

void Finish(WizardState* state) {
  if (state->finishing || !state->options.completion_callback) {
    return;
  }
  state->finishing = true;
  StopCardProbe();
  SetupWizardResult result;
  result.edition = state->edition;
  if (state->edition == app::Edition::kFull) {
    result.maps = state->maps;
    result.wikipedia = state->wikipedia;
    result.nature = state->nature;
  }
  result.open_wifi_settings = state->open_wifi_settings;
  // A copy: the callback deletes this view, and with it the state.
  std::function<bool(const SetupWizardResult&)> callback =
      state->options.completion_callback;
  PlayUiHapticEffect(HapticEffect::kSuccess);
  if (!callback(result)) {
    state->finishing = false;
  }
}

// --- Buttons ----------------------------------------------------------------

void PrimaryClicked(lv_event_t* event) {
  auto* state = static_cast<WizardState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->finishing || state->step_change_pending) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  if (state->step == Step::kDone) {
    Finish(state);
    return;
  }
  if (state->step == Step::kWifi) {
    state->open_wifi_settings = true;
  }
  GoRelative(state, 1);
}

void SecondaryClicked(lv_event_t* event) {
  auto* state = static_cast<WizardState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->finishing) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  if (state->step == Step::kWifi) {
    state->open_wifi_settings = false;
  }
  GoRelative(state, 1);
}

void BackClicked(lv_event_t* event) {
  auto* state = static_cast<WizardState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->finishing) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  GoRelative(state, -1);
}

lv_obj_t* CreateButton(WizardState* state, const char* text, bool primary,
    lv_event_cb_t callback) {
  lv_obj_t* button = lv_button_create(state->page);
  if (button == nullptr) {
    return nullptr;
  }
  lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(button, kButtonRadius, LV_PART_MAIN);
  if (primary) {
    lv_obj_set_size(button, ContentWidth(state), kPrimaryButtonHeight);
    lv_obj_set_pos(button, kSidePadding, PrimaryButtonTop(state));
    lv_obj_set_style_bg_color(button, lv_color_hex(kAccentColor),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(0xC85A0C),
        LV_STATE_PRESSED);
  } else {
    lv_obj_set_size(button, ContentWidth(state), kSecondaryButtonHeight);
    lv_obj_set_pos(button, kSidePadding, SecondaryButtonTop(state));
    lv_obj_set_style_bg_opa(button, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, lv_color_hex(kCardColor),
        LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, LV_STATE_PRESSED);
  }
  AddPressCancelOnLeave(button);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, state);
  lv_obj_t* label = CreateText(button, text,
      primary ? kOnAccentColor : kAccentColor, Font28());
  if (label != nullptr) {
    lv_obj_center(label);
  }
  if (primary) {
    state->primary_label = label;
  }
  return button;
}

// --- Common page parts --------------------------------------------------------

void CreateTopBar(WizardState* state) {
  Step steps[8];
  const size_t count = StepSequence(state, steps, 8);
  size_t index = 0;
  for (size_t i = 0; i < count; ++i) {
    if (steps[i] == state->step) {
      index = i;
    }
  }

  lv_obj_t* back = lv_button_create(state->page);
  if (back != nullptr) {
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(back, kBackButtonSize, kBackButtonSize);
    lv_obj_set_pos(back, 20, kTopBarY);
    lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(kCardColor), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, lv_color_hex(kCardBorderColor),
        LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(back, 0, LV_PART_MAIN);
    AddPressCancelOnLeave(back);
    lv_obj_add_event_cb(back, BackClicked, LV_EVENT_CLICKED, state);
    lv_obj_t* arrow = CreateText(back, icon::kArrowBack, kTextColor,
        &lvgl_font_material_symbols_fill_32);
    if (arrow != nullptr) {
      lv_obj_center(arrow);
    }
  }

  // One dot per step after the welcome page.
  constexpr int kDotSize = 10;
  constexpr int kActiveDotWidth = 30;
  constexpr int kDotGap = 10;
  const int dots = static_cast<int>(count) - 1;
  const int row_width =
      (dots - 1) * (kDotSize + kDotGap) + kActiveDotWidth;
  int x = (Width(state) - row_width) / 2;
  for (int i = 1; i <= dots; ++i) {
    const bool active = static_cast<size_t>(i) == index;
    lv_obj_t* dot = lv_obj_create(state->page);
    if (dot == nullptr) {
      continue;
    }
    MakePlain(dot);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    const int width = active ? kActiveDotWidth : kDotSize;
    lv_obj_set_size(dot, width, kDotSize);
    lv_obj_set_pos(dot, x, kTopBarY + (kBackButtonSize - kDotSize) / 2);
    lv_obj_set_style_radius(dot, kDotSize / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot,
        lv_color_hex(static_cast<size_t>(i) <= index ? kAccentColor
                                                     : kDotColor),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    x += width + kDotGap;
  }
}

// Title and subtitle; returns where the body may start.
int CreateHeader(WizardState* state, const char* title, const char* subtitle) {
  lv_obj_t* title_label =
      CreateText(state->page, title, kTextColor, Font48(), ContentWidth(state));
  if (title_label == nullptr) {
    return kTitleTop;
  }
  lv_obj_set_pos(title_label, kSidePadding, kTitleTop);
  lv_obj_t* last = title_label;
  if (subtitle != nullptr && subtitle[0] != '\0') {
    lv_obj_t* subtitle_label = CreateText(state->page, subtitle,
        kSecondaryTextColor, Font28(), ContentWidth(state));
    if (subtitle_label != nullptr) {
      lv_obj_set_style_text_line_space(subtitle_label, 6, LV_PART_MAIN);
      lv_obj_align_to(subtitle_label, title_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0,
          kSubtitleGap);
      last = subtitle_label;
    }
  }
  lv_obj_update_layout(last);
  return lv_obj_get_y2(last) + kBodyGap;
}

// Scrolling area between the header and the buttons.
lv_obj_t* CreateBody(WizardState* state, int top, int bottom) {
  lv_obj_t* body = lv_obj_create(state->page);
  if (body == nullptr) {
    return nullptr;
  }
  MakePlain(body);
  lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_pos(body, 0, top);
  lv_obj_set_size(body, Width(state), std::max(80, bottom - top));
  return body;
}

void StyleSelectable(lv_obj_t* object, bool selected) {
  lv_obj_set_style_bg_color(object,
      lv_color_hex(selected ? kSelectedCardColor : kCardColor), LV_PART_MAIN);
  lv_obj_set_style_border_color(object,
      lv_color_hex(selected ? kAccentColor : kCardBorderColor), LV_PART_MAIN);
  lv_obj_set_style_border_width(object, selected ? 3 : 2, LV_PART_MAIN);
}

lv_obj_t* CreateCard(lv_obj_t* parent, int x, int y, int width, int height) {
  lv_obj_t* card = lv_obj_create(parent);
  if (card == nullptr) {
    return nullptr;
  }
  MakePlain(card);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, width, height);
  lv_obj_set_style_radius(card, kCardRadius, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  StyleSelectable(card, false);
  return card;
}

// --- Welcome ------------------------------------------------------------------

void BuildWelcome(WizardState* state) {
  const int width = Width(state);
  const int hero_top = std::max(120, Height(state) / 5);

  // A compass rose in a soft orange disc stands in for a logo.
  lv_obj_t* glow = CreateIconCircle(state->page, "", Font22(), 220, 0x1A0C03,
      kAccentColor);
  if (glow != nullptr) {
    lv_obj_set_pos(glow, (width - 220) / 2, hero_top);
    lv_obj_t* disc = CreateIconCircle(glow, icon::kExplore,
        &lvgl_font_material_symbols_fill_56, 150, 0x3A1A04, kAccentColor);
    if (disc != nullptr) {
      lv_obj_center(disc);
    }
  }

  lv_obj_t* title =
      CreateText(state->page, "SurvivalOS", kTextColor, &lvgl_font_google_sans_flex_64);
  if (title != nullptr) {
    lv_obj_set_width(title, ContentWidth(state));
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(title, kSidePadding, hero_top + 220 + 56);
  }
  lv_obj_t* tagline =
      CreateText(state->page, "Your phone, off the grid.", kAccentColor,
          Font36(), ContentWidth(state));
  if (tagline != nullptr && title != nullptr) {
    lv_obj_set_style_text_align(tagline, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align_to(tagline, title, LV_ALIGN_OUT_BOTTOM_MID, 0, 18);
  }
  lv_obj_t* body = CreateText(state->page,
      "Maps, Wikipedia, camera, radio and music that work with no signal at "
      "all. A few questions and it is ready.",
      kSecondaryTextColor, Font28(), ContentWidth(state) - 20);
  if (body != nullptr && tagline != nullptr) {
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_line_space(body, 6, LV_PART_MAIN);
    lv_obj_align_to(body, tagline, LV_ALIGN_OUT_BOTTOM_MID, 0, 28);
  }

  char build[64] = {};
  std::snprintf(build, sizeof(build), "Build %s", app::BuildStamp());
  lv_obj_t* build_label =
      CreateText(state->page, build, 0x5A5A5E, Font22(), ContentWidth(state));
  if (build_label != nullptr) {
    lv_obj_set_style_text_align(build_label, LV_TEXT_ALIGN_CENTER,
        LV_PART_MAIN);
    lv_obj_set_pos(build_label, kSidePadding, SecondaryButtonTop(state) + 20);
  }
  CreateButton(state, "Get started", true, PrimaryClicked);
}

// --- Edition --------------------------------------------------------------------

void ApplyEditionCards(WizardState* state) {
  for (int i = 0; i < 2; ++i) {
    if (state->edition_cards[i] != nullptr) {
      StyleSelectable(state->edition_cards[i],
          (i == 0) == (state->edition == app::Edition::kFull));
    }
  }
}

void EditionCardClicked(lv_event_t* event) {
  auto* state = static_cast<WizardState*>(lv_event_get_user_data(event));
  lv_obj_t* card = lv_event_get_current_target_obj(event);
  if (state == nullptr || card == nullptr) {
    return;
  }
  state->edition = card == state->edition_cards[0] ? app::Edition::kFull
                                                   : app::Edition::kMini;
  PlayUiHapticEffect(HapticEffect::kSelection);
  ApplyEditionCards(state);
}

lv_obj_t* CreateEditionCard(WizardState* state, lv_obj_t* parent, int y,
    const char* symbol, const char* title, const char* text) {
  constexpr int kHeight = 300;
  lv_obj_t* card = CreateCard(parent, kSidePadding, y, ContentWidth(state),
      kHeight);
  if (card == nullptr) {
    return nullptr;
  }
  lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(card, EditionCardClicked, LV_EVENT_CLICKED, state);
  lv_obj_t* circle = CreateIconCircle(card, symbol,
      &lvgl_font_material_symbols_fill_56, 96, 0x3A1A04, kAccentColor);
  if (circle != nullptr) {
    lv_obj_set_pos(circle, 28, 28);
  }
  lv_obj_t* title_label = CreateText(card, title, kTextColor, Font36());
  if (title_label != nullptr) {
    lv_obj_set_pos(title_label, 148, 52);
  }
  lv_obj_t* body = CreateText(card, text, kSecondaryTextColor, Font24(),
      ContentWidth(state) - 56);
  if (body != nullptr) {
    lv_obj_set_style_text_line_space(body, 4, LV_PART_MAIN);
    lv_obj_set_pos(body, 28, 148);
  }
  return card;
}

void BuildEdition(WizardState* state) {
  CreateTopBar(state);
  const int top = CreateHeader(state, "Choose your setup",
      "Both have First Aid, the camera, voice memos, radio and Petdoku. "
      "You can run this setup again from Settings.");
  lv_obj_t* body = CreateBody(state, top, PrimaryButtonTop(state) - 16);
  if (body == nullptr) {
    return;
  }
  state->edition_cards[0] = CreateEditionCard(state, body, 0,
      icon::kSdStorage, "Full",
      "With an SD card. Adds maps, Wikipedia, plants, the translator, music "
      "and files. Photos go to the card.");
  state->edition_cards[1] = CreateEditionCard(state, body, 300 + 24,
      icon::kPhotoLibrary, "Mini",
      "No card needed. Photos and memos stay in the phone's own memory, "
      "which holds about 15 photos.");
  ApplyEditionCards(state);
  CreateButton(state, "Continue", true, PrimaryClicked);
}

// --- SD card ----------------------------------------------------------------------

void UpdateCardStep(WizardState* state) {
  const bool mounted = g_card_probe.mounted.load();
  const bool info = g_card_probe.info_ready.load();
  const int card_state = mounted ? (info ? 2 : 1) : 0;
  if (card_state == state->shown_card_state) {
    return;
  }
  state->shown_card_state = card_state;
  if (state->card_title != nullptr) {
    lv_label_set_text(state->card_title,
        mounted ? "Card ready" : "Looking for a card...");
  }
  if (state->card_detail != nullptr) {
    char detail[96] = "Put a microSD card in the slot. exFAT or FAT32.";
    if (card_state == 1) {
      std::snprintf(detail, sizeof(detail), "Reading its size...");
    } else if (card_state == 2) {
      char total[24] = {};
      char free_text[24] = {};
      FormatGigabytes(
          static_cast<uint64_t>(g_card_probe.total_mib.load()) << 20, total,
          sizeof(total));
      FormatGigabytes(
          static_cast<uint64_t>(g_card_probe.free_mib.load()) << 20, free_text,
          sizeof(free_text));
      std::snprintf(detail, sizeof(detail), "%s, %s free", total, free_text);
    }
    lv_label_set_text(state->card_detail, detail);
  }
  if (state->card_icon_circle != nullptr) {
    lv_obj_set_style_bg_color(state->card_icon_circle,
        lv_color_hex(mounted ? 0x0C3A1A : 0x2C2C2E), LV_PART_MAIN);
    lv_obj_t* icon = lv_obj_get_child(state->card_icon_circle, 0);
    if (icon != nullptr) {
      lv_obj_set_style_text_color(icon,
          lv_color_hex(mounted ? kGoodColor : kSecondaryTextColor),
          LV_PART_MAIN);
    }
  }
  if (state->primary_label != nullptr) {
    lv_label_set_text(state->primary_label,
        mounted ? "Continue" : "Continue without a card");
  }
}

void BuildCard(WizardState* state) {
  CreateTopBar(state);
  const int top = CreateHeader(state, "Insert the SD card",
      "Maps and Wikipedia live on it. Any size works; 8 GB holds Czech "
      "maps and Wikipedia.");
  lv_obj_t* card =
      CreateCard(state->page, kSidePadding, top, ContentWidth(state), 300);
  if (card != nullptr) {
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICKABLE);
    state->card_icon_circle = CreateIconCircle(card, icon::kSdStorage,
        &lvgl_font_material_symbols_fill_56, 120, 0x2C2C2E,
        kSecondaryTextColor);
    if (state->card_icon_circle != nullptr) {
      lv_obj_align(state->card_icon_circle, LV_ALIGN_TOP_MID, 0, 32);
    }
    state->card_title = CreateText(card, "", kTextColor, Font36(),
        ContentWidth(state) - 40);
    if (state->card_title != nullptr) {
      lv_obj_set_style_text_align(state->card_title, LV_TEXT_ALIGN_CENTER,
          LV_PART_MAIN);
      lv_obj_set_pos(state->card_title, 20, 172);
    }
    state->card_detail = CreateText(card, "", kSecondaryTextColor, Font24(),
        ContentWidth(state) - 40);
    if (state->card_detail != nullptr) {
      lv_obj_set_style_text_align(state->card_detail, LV_TEXT_ALIGN_CENTER,
          LV_PART_MAIN);
      lv_obj_set_pos(state->card_detail, 20, 226);
    }
  }
  lv_obj_t* note = CreateText(state->page,
      "Without a card you can still go on and add one later; the phone "
      "picks it up by itself.",
      kSecondaryTextColor, Font24(), ContentWidth(state));
  if (note != nullptr && card != nullptr) {
    lv_obj_set_style_text_line_space(note, 4, LV_PART_MAIN);
    lv_obj_align_to(note, card, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 28);
  }
  CreateButton(state, "Continue", true, PrimaryClicked);
  state->shown_card_state = -1;
  UpdateCardStep(state);
}

// --- Content ----------------------------------------------------------------------

void UpdateContentStep(WizardState* state) {
  for (int i = 0; i < 3; ++i) {
    if (state->map_rows[i] != nullptr) {
      StyleSelectable(
          state->map_rows[i], static_cast<int>(state->maps) == i);
    }
  }
  for (int i = 0; i < 4; ++i) {
    if (state->wikipedia_rows[i] != nullptr) {
      StyleSelectable(
          state->wikipedia_rows[i], static_cast<int>(state->wikipedia) == i);
    }
  }
  for (int i = 0; i < 2; ++i) {
    if (state->nature_rows[i] != nullptr) {
      StyleSelectable(state->nature_rows[i], state->nature == (i == 1));
    }
  }
  if (state->total_label == nullptr) {
    return;
  }
  const uint64_t total = MapsBytes(state->maps) + WikipediaBytes(state->wikipedia) +
      (state->nature ? kNatureBytes : 0);
  char total_text[24] = {};
  FormatGigabytes(total, total_text, sizeof(total_text));
  char text[160] = {};
  uint32_t color = kSecondaryTextColor;
  if (total == 0) {
    std::snprintf(text, sizeof(text),
        "Nothing to download. You can add files to the card any time.");
  } else if (g_card_probe.info_ready.load()) {
    const uint64_t free_bytes =
        static_cast<uint64_t>(g_card_probe.free_mib.load()) << 20;
    char free_text[24] = {};
    FormatGigabytes(free_bytes, free_text, sizeof(free_text));
    if (total <= free_bytes) {
      std::snprintf(text, sizeof(text), "Total %s. Fits: the card has %s free.",
          total_text, free_text);
      color = kGoodColor;
    } else {
      std::snprintf(text, sizeof(text),
          "Total %s, but the card has only %s free.", total_text, free_text);
      color = kAccentColor;
    }
  } else {
    std::snprintf(text, sizeof(text), "Total %s.", total_text);
  }
  lv_label_set_text(state->total_label, text);
  lv_obj_set_style_text_color(state->total_label, lv_color_hex(color),
      LV_PART_MAIN);
}

void MapRowClicked(lv_event_t* event) {
  auto* state = static_cast<WizardState*>(lv_event_get_user_data(event));
  lv_obj_t* row = lv_event_get_current_target_obj(event);
  if (state == nullptr || row == nullptr) {
    return;
  }
  for (int i = 0; i < 3; ++i) {
    if (row == state->map_rows[i]) {
      state->maps = static_cast<app::MapsChoice>(i);
    }
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  UpdateContentStep(state);
}

void NatureRowClicked(lv_event_t* event) {
  auto* state = static_cast<WizardState*>(lv_event_get_user_data(event));
  lv_obj_t* row = lv_event_get_current_target_obj(event);
  if (state == nullptr || row == nullptr) {
    return;
  }
  state->nature = row == state->nature_rows[1];
  PlayUiHapticEffect(HapticEffect::kSelection);
  UpdateContentStep(state);
}

void WikipediaRowClicked(lv_event_t* event) {
  auto* state = static_cast<WizardState*>(lv_event_get_user_data(event));
  lv_obj_t* row = lv_event_get_current_target_obj(event);
  if (state == nullptr || row == nullptr) {
    return;
  }
  for (int i = 0; i < 4; ++i) {
    if (row == state->wikipedia_rows[i]) {
      state->wikipedia = static_cast<app::WikipediaChoice>(i);
    }
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  UpdateContentStep(state);
}

lv_obj_t* CreateOptionRow(WizardState* state, lv_obj_t* parent, int y,
    const char* title, uint64_t bytes, lv_event_cb_t callback) {
  lv_obj_t* row = CreateCard(parent, kSidePadding, y, ContentWidth(state),
      kOptionRowHeight);
  if (row == nullptr) {
    return nullptr;
  }
  lv_obj_set_style_radius(row, 22, LV_PART_MAIN);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(row, callback, LV_EVENT_CLICKED, state);
  lv_obj_t* title_label = CreateText(row, title, kTextColor, Font28());
  if (title_label != nullptr) {
    lv_obj_align(title_label, LV_ALIGN_LEFT_MID, 26, 0);
  }
  if (bytes > 0) {
    char size[24] = {};
    FormatGigabytes(bytes, size, sizeof(size));
    lv_obj_t* size_label = CreateText(row, size, kSecondaryTextColor, Font24());
    if (size_label != nullptr) {
      lv_obj_align(size_label, LV_ALIGN_RIGHT_MID, -26, 0);
    }
  }
  return row;
}

lv_obj_t* CreateSectionTitle(lv_obj_t* parent, const char* text, int y) {
  lv_obj_t* label = CreateText(parent, text, kSecondaryTextColor, Font22());
  if (label != nullptr) {
    lv_obj_set_style_text_letter_space(label, 2, LV_PART_MAIN);
    lv_obj_set_pos(label, kSidePadding + 8, y);
  }
  return label;
}

void BuildContent(WizardState* state) {
  CreateTopBar(state);
  const int top = CreateHeader(state, "What goes on the card?",
      "Pick now, download on a computer later.");
  state->total_label = CreateText(state->page, "", kSecondaryTextColor,
      Font24(), ContentWidth(state));
  if (state->total_label != nullptr) {
    lv_obj_set_pos(state->total_label, kSidePadding,
        PrimaryButtonTop(state) - 76);
  }
  lv_obj_t* body = CreateBody(state, top - 16, PrimaryButtonTop(state) - 90);
  if (body == nullptr) {
    return;
  }
  int y = 0;
  CreateSectionTitle(body, "MAPS", y);
  y += 40;
  const char* map_titles[] = {"No maps", "Czechia, detailed",
      "Whole world, overview"};
  const uint64_t map_sizes[] = {0, kMapsCzechiaBytes, kMapsWorldBytes};
  for (int i = 0; i < 3; ++i) {
    state->map_rows[i] = CreateOptionRow(
        state, body, y, map_titles[i], map_sizes[i], MapRowClicked);
    y += kOptionRowHeight + kOptionRowGap;
  }
  y += 22;
  CreateSectionTitle(body, "WIKIPEDIA (TEXT, NO PICTURES)", y);
  y += 40;
  const char* wikipedia_titles[] = {"No Wikipedia", "Czech", "English",
      "Czech and English"};
  const uint64_t wikipedia_sizes[] = {0, kWikipediaCzechBytes,
      kWikipediaEnglishBytes, kWikipediaCzechBytes + kWikipediaEnglishBytes};
  for (int i = 0; i < 4; ++i) {
    state->wikipedia_rows[i] = CreateOptionRow(state, body, y,
        wikipedia_titles[i], wikipedia_sizes[i], WikipediaRowClicked);
    y += kOptionRowHeight + kOptionRowGap;
  }
  y += 22;
  CreateSectionTitle(body, "FIELD GUIDE", y);
  y += 40;
  state->nature_rows[0] = CreateOptionRow(state, body, y, "No field guide", 0,
      NatureRowClicked);
  y += kOptionRowHeight + kOptionRowGap;
  state->nature_rows[1] = CreateOptionRow(state, body, y,
      "World plants & fungi", kNatureBytes, NatureRowClicked);
  CreateButton(state, "Continue", true, PrimaryClicked);
  UpdateContentStep(state);
}

// --- Files --------------------------------------------------------------------------

void BuildFiles(WizardState* state) {
  CreateTopBar(state);
  const int top = CreateHeader(state, "Get the files",
      "Open this page on a computer. Your picks are already ticked there.");
  lv_obj_t* body = CreateBody(state, top - 10, PrimaryButtonTop(state) - 16);
  if (body == nullptr) {
    return;
  }

  char url[160] = {};
  FormatSetupContentUrl(state->maps, state->wikipedia, state->nature, url, sizeof(url));
  // The QR code sits on a white tile; scanners want the light quiet zone.
  lv_obj_t* tile = lv_obj_create(body);
  int y = 0;
  if (tile != nullptr) {
    MakePlain(tile);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(tile, kQrSize + 40, kQrSize + 40);
    lv_obj_set_pos(tile, (Width(state) - kQrSize - 40) / 2, 0);
    lv_obj_set_style_bg_color(tile, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(tile, 24, LV_PART_MAIN);
    lv_obj_t* qr = lv_qrcode_create(tile);
    if (qr != nullptr) {
      lv_qrcode_set_size(qr, kQrSize);
      lv_qrcode_set_dark_color(qr, lv_color_hex(0x000000));
      lv_qrcode_set_light_color(qr, lv_color_hex(0xFFFFFF));
      if (lv_qrcode_update(qr, url, std::strlen(url)) != LV_RESULT_OK) {
        LogMessage(LogLevel::kWarning, __FILE__, __LINE__,
            "QR code for %s failed\n", url);
      }
      lv_obj_center(qr);
    }
    y = kQrSize + 40 + 20;
  }
  lv_obj_t* link = CreateText(body, kContentPageShownUrl, kAccentColor,
      Font24(), ContentWidth(state));
  if (link != nullptr) {
    lv_obj_set_style_text_align(link, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_pos(link, kSidePadding, y);
    y += 64;
  }

  const char* steps[] = {
      "Download the files there.",
      "Copy them to the card, into any folder.",
      "Put the card back. The phone sorts the files by itself.",
  };
  for (int i = 0; i < 3; ++i) {
    char number[4] = {};
    std::snprintf(number, sizeof(number), "%d", i + 1);
    lv_obj_t* circle = CreateIconCircle(body, number, Font24(), 44,
        0x3A1A04, kAccentColor);
    if (circle != nullptr) {
      lv_obj_set_pos(circle, kSidePadding, y);
    }
    lv_obj_t* text = CreateText(body, steps[i], kTextColor, Font24(),
        ContentWidth(state) - 64);
    if (text != nullptr) {
      lv_obj_set_pos(text, kSidePadding + 64, y + 8);
      lv_obj_update_layout(text);
      y += std::max(44, static_cast<int>(lv_obj_get_height(text)) + 8) + 22;
    } else {
      y += 66;
    }
  }
  CreateButton(state, "Continue", true, PrimaryClicked);
}

// --- Wi-Fi --------------------------------------------------------------------------

void BuildWifi(WizardState* state) {
  CreateTopBar(state);
  const int top = CreateHeader(state, "Wi-Fi",
      "The phone needs no network. Wi-Fi only sets the clock and stays off "
      "otherwise, which saves battery.");
  lv_obj_t* circle = CreateIconCircle(state->page, icon::kSignalWifi4Bar,
      &lvgl_font_material_symbols_outline_56, 160, 0x3A1A04, kAccentColor);
  if (circle != nullptr) {
    const int space = SecondaryButtonTop(state) - top;
    lv_obj_set_pos(circle, (Width(state) - 160) / 2,
        top + std::max(0, (space - 160) / 2 - 40));
  }
  CreateButton(state, "Not now", false, SecondaryClicked);
  CreateButton(state, "Set up Wi-Fi", true, PrimaryClicked);
}

// --- Done ---------------------------------------------------------------------------

void BuildDone(WizardState* state) {
  CreateTopBar(state);
  const int top = CreateHeader(state, "You're all set",
      state->open_wifi_settings
          ? "Wi-Fi settings open next. A few things worth knowing:"
          : "A few things worth knowing:");
  lv_obj_t* body = CreateBody(state, top - 10, PrimaryButtonTop(state) - 16);
  if (body == nullptr) {
    return;
  }
  struct Tip {
    const char* symbol;
    const char* text;
  };
  const Tip tips[] = {
      {icon::kKeyboardArrowUp,
          "Swipe up from the bottom edge to go home."},
      {icon::kKeyboardArrowDown,
          "Swipe down from the top edge for Control Center."},
      {icon::kEnergySavingsLeaf,
          "The leaf in Control Center is Low Power Mode. It also turns on by "
          "itself at 20%."},
      {icon::kSdStorage,
          "Files copied to the card are sorted into place when it goes in."},
  };
  int y = 0;
  const size_t tip_count = state->edition == app::Edition::kFull ? 4 : 3;
  for (size_t i = 0; i < tip_count; ++i) {
    lv_obj_t* circle = CreateIconCircle(body, tips[i].symbol,
        &lvgl_font_material_symbols_fill_32, 64, kCardColor, kAccentColor);
    if (circle != nullptr) {
      lv_obj_set_pos(circle, kSidePadding, y);
    }
    lv_obj_t* text = CreateText(body, tips[i].text, kTextColor, Font24(),
        ContentWidth(state) - 88);
    if (text != nullptr) {
      lv_obj_set_style_text_line_space(text, 4, LV_PART_MAIN);
      lv_obj_set_pos(text, kSidePadding + 88, y + 14);
      lv_obj_update_layout(text);
      y += std::max(64, static_cast<int>(lv_obj_get_height(text)) + 14) + 30;
    } else {
      y += 94;
    }
  }
  CreateButton(state, "Start using SurvivalOS", true, PrimaryClicked);
}

// --- Plumbing -----------------------------------------------------------------------

void StatusTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<WizardState*>(lv_timer_get_user_data(timer));
  if (state == nullptr) {
    return;
  }
  if (state->step == Step::kCard) {
    UpdateCardStep(state);
  } else if (state->step == Step::kContent) {
    const int card_state = g_card_probe.info_ready.load() ? 2 : 0;
    if (card_state != state->shown_card_state) {
      state->shown_card_state = card_state;
      UpdateContentStep(state);
    }
  }
}

void ShowStep(WizardState* state, Step step) {
  state->step = step;
  state->card_icon_circle = nullptr;
  state->card_title = nullptr;
  state->card_detail = nullptr;
  state->primary_label = nullptr;
  state->total_label = nullptr;
  std::fill(std::begin(state->map_rows), std::end(state->map_rows), nullptr);
  std::fill(std::begin(state->wikipedia_rows), std::end(state->wikipedia_rows),
      nullptr);
  std::fill(std::begin(state->edition_cards), std::end(state->edition_cards),
      nullptr);
  state->shown_card_state = -1;
  lv_obj_clean(state->page);

  // The card is only looked for while a page needs it, so a phone without
  // one does not spend the whole assistant retrying the mount.
  if (step == Step::kCard || step == Step::kContent) {
    StartCardProbe(state->options.storage);
  } else {
    StopCardProbe();
  }

  switch (step) {
    case Step::kWelcome:
      BuildWelcome(state);
      break;
    case Step::kEdition:
      BuildEdition(state);
      break;
    case Step::kCard:
      BuildCard(state);
      break;
    case Step::kContent:
      BuildContent(state);
      break;
    case Step::kFiles:
      BuildFiles(state);
      break;
    case Step::kWifi:
      BuildWifi(state);
      break;
    case Step::kDone:
      BuildDone(state);
      break;
  }
}

void WizardDeleteCallback(lv_event_t* event) {
  auto* state = static_cast<WizardState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  StopCardProbe();
  lv_async_call_cancel(ApplyPendingStep, state);
  if (state->status_timer != nullptr) {
    lv_timer_delete(state->status_timer);
  }
  delete state;
}

}  // namespace

void FormatSetupContentUrl(app::MapsChoice maps,
    app::WikipediaChoice wikipedia, bool nature, char* buffer, size_t size) {
  if (buffer == nullptr || size == 0) {
    return;
  }
  const char* maps_value = maps == app::MapsChoice::kCzechia ? "cz"
      : maps == app::MapsChoice::kWorld                     ? "world"
                                                            : "none";
  const char* wikipedia_value = wikipedia == app::WikipediaChoice::kCzech ? "cs"
      : wikipedia == app::WikipediaChoice::kEnglish                       ? "en"
      : wikipedia == app::WikipediaChoice::kBoth ? "cs,en"
                                                 : "none";
  std::snprintf(buffer, size, "%s#content?maps=%s&wiki=%s&nature=%d",
      kContentPageUrl, maps_value, wikipedia_value, nature ? 1 : 0);
}

lv_obj_t* CreateSetupWizardView(
    lv_obj_t* parent, const SetupWizardViewOptions& options) {
  if (parent == nullptr || options.screen_width <= 0 ||
      options.screen_height <= 0 || !options.completion_callback) {
    return nullptr;
  }
  auto* state = new (std::nothrow) WizardState();
  if (state == nullptr) {
    return nullptr;
  }
  state->options = options;
  state->edition = app::GetEdition();

  lv_obj_t* root = lv_obj_create(parent);
  if (root == nullptr) {
    delete state;
    return nullptr;
  }
  state->root = root;
  MakePlain(root);
  lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(root, options.screen_width, options.screen_height);
  lv_obj_set_pos(root, 0, 0);
  lv_obj_set_style_bg_color(root, lv_color_hex(kBackgroundColor), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_add_event_cb(root, WizardDeleteCallback, LV_EVENT_DELETE, state);

  state->page = lv_obj_create(root);
  if (state->page == nullptr) {
    lv_obj_delete(root);
    return nullptr;
  }
  MakePlain(state->page);
  lv_obj_set_size(state->page, options.screen_width, options.screen_height);
  lv_obj_set_pos(state->page, 0, 0);

  state->status_timer =
      lv_timer_create(StatusTimerCallback, kStatusRefreshMs, state);
  ShowStep(state, Step::kWelcome);
  lv_obj_move_to_index(root, -1);
  return root;
}

}  // namespace lilygo_box::ui
