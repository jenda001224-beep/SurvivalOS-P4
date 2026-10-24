/*
 * @Description: SurvivalOS First Aid: what to do, the medicine cabinet,
 *               doses for each person and the herbs that help
 * @License: GPL 3.0
 *
 * Home: what to deal with first, a search by symptom or injury, the cabinet and the
 * person the doses are for, and every guide by category. A guide shows the danger
 * signs, the steps, what not to do, the medicines from the cabinet that
 * help with the dose for the chosen person and what is left, and the herbs
 * that help with a photo from the field guide and how to prepare them.
 *
 * Content: firstaid/firstaid_*.cpp (built into the firmware, works without a
 * card). Cabinet and people: firstaid.txt in internal memory.
 */
#include "ui/views/firstaid_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <new>
#include <string>
#include <strings.h>
#include <vector>

#include "esp_heap_caps.h"
#include "firstaid/firstaid_content.h"
#include "firstaid/firstaid_model.h"
#include "hal/providers/rtc_provider.h"
#include "hal/providers/storage_provider.h"
#include "misc/cache/instance/lv_image_cache.h"
#include "survival/jpeg_image.h"
#include "survival/nature_world.h"
#include "ui/haptic_feedback.h"
#include "ui/input/back_navigation_controller.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"
#include "ui/widgets/shared_keyboard.h"

namespace lilygo_box::ui {
namespace {

using firstaid::Cabinet;
using firstaid::CabinetItem;
using firstaid::Category;
using firstaid::Form;
using firstaid::Medicine;
using firstaid::Person;
using firstaid::Topic;
using firstaid::Urgency;

constexpr int kSide = 18;
constexpr int kTitleTop = 58;
constexpr int kContentTop = 120;
constexpr int kPlantPhoto = 128;
constexpr uint32_t kSearchPauseMs = 300;

constexpr uint32_t kRed = 0xFF453A;
constexpr uint32_t kRedDark = 0x3A0E0C;
constexpr uint32_t kAmber = 0xFFB340;
constexpr uint32_t kAmberDark = 0x33240A;
constexpr uint32_t kGreen = 0x30D158;
constexpr uint32_t kCard = 0x1C1C1E;
constexpr uint32_t kChip = 0x2C2C2E;
constexpr uint32_t kMuted = 0x8E8E93;
constexpr uint32_t kText = 0xF2F2F7;
constexpr uint32_t kBody = 0xD1D1D6;

struct FirstAidState {
  int width = 0;
  int height = 0;
  hal::StorageProvider* storage = nullptr;
  hal::RtcProvider* rtc = nullptr;
  lv_obj_t* root = nullptr;
  lv_obj_t* home_list = nullptr;
  lv_obj_t* search = nullptr;
  lv_obj_t* keyboard = nullptr;
  lv_obj_t* cabinet_button_label = nullptr;
  lv_obj_t* person_button_label = nullptr;
  lv_timer_t* search_timer = nullptr;
  std::vector<lv_obj_t*> pages;
  Cabinet cabinet;
  survival::NatureWorld nature;
  bool nature_tried = false;
  int year = 0;
  int month = 0;
  // Item editor.
  int edit_index = -1;
  std::string edit_medicine;
  Form edit_form = Form::kTablet;
  lv_obj_t* edit_name = nullptr;
  lv_obj_t* edit_strength = nullptr;
  lv_obj_t* edit_quantity = nullptr;
  lv_obj_t* edit_month = nullptr;
  lv_obj_t* edit_year = nullptr;
  lv_obj_t* edit_note = nullptr;
  // Person editor.
  int person_index = -1;
  lv_obj_t* person_name = nullptr;
  lv_obj_t* person_age = nullptr;
  lv_obj_t* person_weight = nullptr;
  lv_obj_t* person_height = nullptr;
  // The guide on screen, to redraw after the person changes.
  const Topic* open_topic = nullptr;
};

// Decoded plant photo owned by its image object.
struct PhotoHolder {
  survival::JpegImage image;
  lv_image_dsc_t descriptor = {};
};

// ── Helpers ─────────────────────────────────────────────────────────────────

void MakePlain(lv_obj_t* object) {
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(object, 0, LV_PART_MAIN);
}

lv_obj_t* Label(lv_obj_t* parent, const char* text, uint32_t color, const lv_font_t* font,
    int width = 0) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, text != nullptr ? text : "");
  lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  if (width > 0) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  }
  lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
  return label;
}

lv_obj_t* Column(lv_obj_t* parent, int width, int gap) {
  lv_obj_t* column = lv_obj_create(parent);
  MakePlain(column);
  lv_obj_set_size(column, width, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(column, gap, LV_PART_MAIN);
  lv_obj_remove_flag(column, LV_OBJ_FLAG_CLICKABLE);
  return column;
}

lv_obj_t* Card(lv_obj_t* parent, int width, uint32_t color = kCard, uint32_t border = 0) {
  lv_obj_t* card = Column(parent, width, 8);
  lv_obj_set_style_bg_color(card, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 22, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
  if (border != 0) {
    lv_obj_set_style_border_width(card, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(border), LV_PART_MAIN);
  }
  return card;
}

lv_obj_t* Button(lv_obj_t* parent, const char* text, uint32_t color, lv_event_cb_t callback,
    void* user, int width = LV_SIZE_CONTENT, int height = 72) {
  lv_obj_t* button = lv_button_create(parent);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_radius(button, 20, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_pad_hor(button, 20, LV_PART_MAIN);
  lv_obj_t* label = Label(button, text, 0xFFFFFF, &lvgl_font_google_sans_flex_24);
  lv_obj_center(label);
  if (callback != nullptr) {
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user);
  }
  return button;
}

// Lines of a '\n' separated field.
std::vector<std::string> Lines(const char* text) {
  std::vector<std::string> lines;
  if (text == nullptr) {
    return lines;
  }
  std::string current;
  for (const char* p = text;; ++p) {
    if (*p == '\n' || *p == '\0') {
      if (!current.empty()) lines.push_back(current);
      current.clear();
      if (*p == '\0') break;
    } else {
      current.push_back(*p);
    }
  }
  return lines;
}

std::vector<std::string> Words(const char* text) {
  std::vector<std::string> words;
  std::string current;
  for (const char* p = text != nullptr ? text : "";; ++p) {
    if (*p == ' ' || *p == '\0') {
      if (!current.empty()) words.push_back(current);
      current.clear();
      if (*p == '\0') break;
    } else {
      current.push_back(*p);
    }
  }
  return words;
}

std::string Num(float value) {
  char buffer[24];
  if (std::fabs(value - std::round(value)) < 0.05f) {
    std::snprintf(buffer, sizeof(buffer), "%.0f", value);
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.1f", value);
    for (char* p = buffer; *p; ++p) if (*p == '.') *p = ',';
  }
  return buffer;
}

float ParseNumber(const char* text) {
  std::string copy = text != nullptr ? text : "";
  std::replace(copy.begin(), copy.end(), ',', '.');
  return static_cast<float>(std::atof(copy.c_str()));
}

uint32_t UrgencyColor(Urgency urgency) {
  return urgency == Urgency::kEmergency ? kRed : urgency == Urgency::kDoctor ? kAmber : kGreen;
}

const char* UrgencyText(Urgency urgency) {
  return urgency == Urgency::kEmergency ? "Ohrožení života – jednej hned"
       : urgency == Urgency::kDoctor    ? "Vážné – ošetři, pak k lékaři"
                                        : "Zvládneš sám";
}

std::string PersonSummary(const Person& person) {
  return person.name + " · " + Num(person.age_years) + " let · " + Num(person.weight_kg) + " kg";
}

void Haptic() { PlayUiHapticEffect(HapticEffect::kSelection); }

// ── Pages ───────────────────────────────────────────────────────────────────

void PopPage(FirstAidState* state);

void KeyboardFor(FirstAidState* state, lv_obj_t* field, bool numeric) {
  if (state->keyboard == nullptr || field == nullptr) {
    return;
  }
  AttachSharedKeyboardToTextArea(state->keyboard, field, numeric ? "0123456789.," : nullptr,
      numeric ? LV_KEYBOARD_MODE_NUMBER : LV_KEYBOARD_MODE_USER_1);
  lv_obj_move_foreground(state->keyboard);
}

void FieldFocused(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  lv_obj_t* field = lv_event_get_target_obj(event);
  const bool numeric = lv_obj_get_user_data(field) != nullptr;
  KeyboardFor(state, field, numeric);
}

lv_obj_t* PushPage(FirstAidState* state, const char* title, lv_obj_t** body) {
  lv_obj_t* page = lv_obj_create(state->root);
  MakePlain(page);
  lv_obj_add_flag(page, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(page, state->width, state->height);
  lv_obj_set_pos(page, 0, 0);
  lv_obj_set_style_bg_color(page, lv_color_hex(theme::ActiveThemeColors().surface), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(page, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_t* back = lv_button_create(page);
  lv_obj_set_size(back, 72, 72);
  lv_obj_set_pos(back, kSide - 6, kTitleTop - 14);
  lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(back, lv_color_hex(kChip), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(back, [](lv_event_t* event) {
    PopPage(static_cast<FirstAidState*>(lv_event_get_user_data(event)));
  }, LV_EVENT_CLICKED, state);
  lv_obj_t* arrow = Label(back, icon::kArrowBack, 0xFFFFFF, &lvgl_font_material_symbols_fill_32);
  lv_obj_center(arrow);
  lv_obj_t* heading = Label(page, title, kText, &lvgl_font_google_sans_flex_32,
      state->width - kSide - 100);
  lv_label_set_long_mode(heading, LV_LABEL_LONG_DOT);
  lv_obj_set_height(heading, 44);
  lv_obj_set_pos(heading, kSide + 84, kTitleTop);
  lv_obj_t* scroll = lv_obj_create(page);
  MakePlain(scroll);
  lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
  lv_obj_set_size(scroll, state->width, state->height - kContentTop);
  lv_obj_set_pos(scroll, 0, kContentTop);
  lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(scroll, 14, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(scroll, 80, LV_PART_MAIN);
  state->pages.push_back(page);
  if (state->keyboard != nullptr) {
    HideSharedKeyboard(state->keyboard);
    lv_obj_move_foreground(state->keyboard);
  }
  // The back gesture is handled by the app's own handler, which pops pages:
  // a handler on the page would be destroyed while it runs.
  *body = scroll;
  return page;
}

void RefreshHomeButtons(FirstAidState* state);
void ShowTopic(FirstAidState* state, const Topic* topic);

void PopPage(FirstAidState* state) {
  if (state->pages.empty()) {
    return;
  }
  lv_obj_t* page = state->pages.back();
  state->pages.pop_back();
  if (state->keyboard != nullptr) {
    AttachSharedKeyboardToTextArea(state->keyboard, state->search, nullptr);
    HideSharedKeyboard(state->keyboard);
  }
  // Later, not now: this often runs from a button on the page itself.
  lv_obj_delete_async(page);
  if (state->pages.empty()) {
    state->open_topic = nullptr;
  }
  RefreshHomeButtons(state);
}

// ── Plants ──────────────────────────────────────────────────────────────────

bool OpenNature(FirstAidState* state) {
  if (state->nature_tried) {
    return state->nature.IsOpen();
  }
  state->nature_tried = true;
  if (state->storage == nullptr ||
      (!state->storage->IsSdCardMounted() && !state->storage->EnsureSdCardMounted())) {
    return false;
  }
  char directory[96];
  std::snprintf(directory, sizeof(directory), "%s/nature", state->storage->SdCardBasePath());
  char path[200] = {};
  return survival::FindNatureWorld(directory, path, sizeof(path)) && state->nature.Open(path);
}

void PhotoDeleted(lv_event_t* event) {
  auto* holder = static_cast<PhotoHolder*>(lv_event_get_user_data(event));
  if (holder != nullptr) {
    lv_image_cache_drop(&holder->descriptor);
    survival::FreeJpegImage(&holder->image);
    delete holder;
  }
}

// The first photo of a species in the field guide, by Latin name.
lv_obj_t* PlantPhoto(FirstAidState* state, lv_obj_t* parent, const char* latin) {
  if (!OpenNature(state)) {
    return nullptr;
  }
  const int species = state->nature.FindLatin(latin);
  survival::NatureRecord record;
  if (species < 0 || !state->nature.Record(static_cast<uint32_t>(species), &record) ||
      state->nature.Attributes(static_cast<uint32_t>(species)).photo_count == 0) {
    return nullptr;
  }
  size_t size = 0;
  uint8_t* jpeg = state->nature.ReadPhoto(record.first_photo, &size);
  if (jpeg == nullptr) {
    return nullptr;
  }
  auto* holder = new (std::nothrow) PhotoHolder();
  const bool ok = holder != nullptr &&
      survival::DecodeJpegThumbnail(jpeg, size, kPlantPhoto, &holder->image);
  heap_caps_free(jpeg);
  if (!ok) {
    delete holder;
    return nullptr;
  }
  holder->descriptor.header.magic = LV_IMAGE_HEADER_MAGIC;
  holder->descriptor.header.cf = LV_COLOR_FORMAT_RGB565;
  holder->descriptor.header.w = holder->image.width;
  holder->descriptor.header.h = holder->image.height;
  holder->descriptor.header.stride = holder->image.width * 2;
  holder->descriptor.data = holder->image.pixels;
  holder->descriptor.data_size = holder->image.width * holder->image.height * 2;
  lv_obj_t* image = lv_image_create(parent);
  lv_obj_add_event_cb(image, PhotoDeleted, LV_EVENT_DELETE, holder);
  lv_image_set_src(image, &holder->descriptor);
  lv_obj_set_style_radius(image, 16, LV_PART_MAIN);
  lv_obj_set_style_clip_corner(image, true, LV_PART_MAIN);
  return image;
}

// ── Guide page ──────────────────────────────────────────────────────────────

void TopicButtonClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  lv_obj_t* button = lv_event_get_current_target_obj(event);
  const auto index = reinterpret_cast<intptr_t>(lv_obj_get_user_data(button));
  Haptic();
  ShowTopic(state, &firstaid::TopicAt(static_cast<size_t>(index)));
}

void CyclePersonClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  const int count = static_cast<int>(state->cabinet.people().size());
  state->cabinet.set_selected_person((state->cabinet.selected_person() + 1) % std::max(1, count));
  state->cabinet.Save();
  Haptic();
  // Redraw the guide for the new person.
  const Topic* topic = state->open_topic;
  if (topic != nullptr && !state->pages.empty()) {
    lv_obj_t* page = state->pages.back();
    state->pages.pop_back();
    lv_obj_delete_async(page);
    ShowTopic(state, topic);
  }
}

void ShowCabinet(FirstAidState* state);
void ShowAddItem(FirstAidState* state);

void MedicinesSection(FirstAidState* state, lv_obj_t* body, const Topic& topic, int width) {
  const std::vector<std::string> keys = Words(topic.medicines);
  if (keys.empty()) {
    return;
  }
  Label(body, "Léky", kMuted, &lvgl_font_google_sans_flex_22, width);
  const Person& person = state->cabinet.Current();
  lv_obj_t* chip = Button(body, ("Pro: " + PersonSummary(person) + "  (změnit)").c_str(), kChip,
      CyclePersonClicked, state, width, 64);
  (void)chip;
  for (const std::string& key : keys) {
    const Medicine* medicine = firstaid::FindMedicine(key.c_str());
    if (medicine == nullptr) {
      continue;
    }
    const std::vector<size_t> items = state->cabinet.ItemsFor(key.c_str());
    if (items.empty()) {
      lv_obj_t* card = Card(body, width, kCard);
      Label(card, medicine->name, kText, &lvgl_font_google_sans_flex_28, width - 36);
      Label(card, ("Nemáš v lékárničce. Hledej: " + std::string(medicine->brands)).c_str(), kMuted,
          &lvgl_font_google_sans_flex_22, width - 36);
      const firstaid::DoseAdvice advice = firstaid::CalculateDose(*medicine, person, nullptr);
      Label(card, advice.text.c_str(), advice.allowed ? kBody : kAmber,
          &lvgl_font_google_sans_flex_22, width - 36);
      continue;
    }
    for (size_t index : items) {
      const CabinetItem& item = state->cabinet.items()[index];
      const int expiry = firstaid::ExpiryState(item, state->year, state->month);
      lv_obj_t* card = Card(body, width, kCard, expiry == 2 ? kRed : 0x2E7D32);
      Label(card, (item.name + "  ·  " + medicine->name).c_str(), kText,
          &lvgl_font_google_sans_flex_28, width - 36);
      std::string stock = "Zbývá " + Num(item.quantity) + " " + firstaid::FormName(item.form);
      if (expiry == 2) stock += "  ·  PO EXPIRACI";
      else if (expiry == 1) stock += "  ·  brzy expiruje";
      Label(card, stock.c_str(), expiry == 2 ? kRed : kGreen, &lvgl_font_google_sans_flex_22,
          width - 36);
      const firstaid::DoseAdvice advice = firstaid::CalculateDose(*medicine, person, &item);
      Label(card, advice.text.c_str(), advice.allowed ? kText : kAmber,
          &lvgl_font_google_sans_flex_24, width - 36);
      if (advice.has_numbers && advice.units > 0 && item.quantity > 0) {
        const float doses = std::floor(item.quantity / advice.units + 0.01f);
        Label(card, ("Vystačí na " + Num(doses) + " dávek.").c_str(), kMuted,
            &lvgl_font_google_sans_flex_22, width - 36);
      }
      if (medicine->warnings[0] != '\0') {
        Label(card, medicine->warnings, kAmber, &lvgl_font_google_sans_flex_22, width - 36);
      }
    }
  }
  Label(body, "Dávky jsou z příbalových letáků. Vždy si leták přečti; u dětí, těhotných, "
      "nemocných a starších lidí se poraď s lékařem nebo lékárníkem.", kMuted,
      &lvgl_font_google_sans_flex_22, width);
}

void PlantsSection(FirstAidState* state, lv_obj_t* body, const Topic& topic, int width) {
  const std::vector<std::string> lines = Lines(topic.plants);
  if (lines.empty()) {
    return;
  }
  Label(body, "Pomůže i příroda", kMuted, &lvgl_font_google_sans_flex_22, width);
  for (const std::string& line : lines) {
    const size_t first = line.find('|');
    const size_t second = first == std::string::npos ? std::string::npos : line.find('|', first + 1);
    if (second == std::string::npos) {
      continue;
    }
    const std::string latin = line.substr(0, first);
    const std::string czech = line.substr(first + 1, second - first - 1);
    const std::string use = line.substr(second + 1);
    lv_obj_t* card = lv_obj_create(body);
    MakePlain(card);
    lv_obj_set_size(card, width, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x16261A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(card, 22, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 16, LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(card, 16, LV_PART_MAIN);
    lv_obj_t* photo = PlantPhoto(state, card, latin.c_str());
    const int text_width = width - 32 - (photo != nullptr ? kPlantPhoto + 16 : 0);
    lv_obj_t* text = Column(card, text_width, 4);
    Label(text, czech.c_str(), kText, &lvgl_font_google_sans_flex_28, text_width);
    Label(text, latin.c_str(), kMuted, &lvgl_font_google_sans_flex_22, text_width);
    Label(text, use.c_str(), kBody, &lvgl_font_google_sans_flex_22, text_width);
  }
  Label(body, "Byliny nejsou náhrada léků u vážných stavů. Sbírej jen rostliny, které bezpečně "
      "znáš (aplikace Nature), ne u silnic a postřiků.", kMuted, &lvgl_font_google_sans_flex_22,
      width);
}

void ShowTopic(FirstAidState* state, const Topic* topic) {
  if (topic == nullptr) {
    return;
  }
  state->open_topic = topic;
  lv_obj_t* body = nullptr;
  PushPage(state, topic->title, &body);
  const int width = state->width - 2 * kSide;

  lv_obj_t* badge = Button(body, UrgencyText(topic->urgency), UrgencyColor(topic->urgency),
      nullptr, nullptr, width, 60);
  lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(badge, LV_OPA_30, LV_PART_MAIN);

  if (topic->call_when[0] != '\0') {
    lv_obj_t* card = Card(body, width, kRedDark, kRed);
    Label(card, "Vážné, když:", kRed, &lvgl_font_google_sans_flex_24, width - 40);
    Label(card, topic->call_when, kText, &lvgl_font_google_sans_flex_24, width - 40);
  }

  Label(body, "Co dělat", kMuted, &lvgl_font_google_sans_flex_22, width);
  int number = 1;
  for (const std::string& step : Lines(topic->steps)) {
    lv_obj_t* row = lv_obj_create(body);
    MakePlain(row);
    lv_obj_set_size(row, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 14, LV_PART_MAIN);
    lv_obj_t* circle = lv_obj_create(row);
    MakePlain(circle);
    lv_obj_set_size(circle, 46, 46);
    lv_obj_set_style_radius(circle, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(circle, lv_color_hex(UrgencyColor(topic->urgency)), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(circle, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t* digit = Label(circle, std::to_string(number++).c_str(), 0x000000,
        &lvgl_font_google_sans_flex_24);
    lv_obj_center(digit);
    Label(row, step.c_str(), kText, &lvgl_font_google_sans_flex_28, width - 60);
  }

  if (topic->avoid[0] != '\0') {
    lv_obj_t* card = Card(body, width, kAmberDark, kAmber);
    Label(card, "Nedělej", kAmber, &lvgl_font_google_sans_flex_24, width - 40);
    for (const std::string& line : Lines(topic->avoid)) {
      Label(card, ("– " + line).c_str(), kText, &lvgl_font_google_sans_flex_24, width - 40);
    }
  }

  MedicinesSection(state, body, *topic, width);
  PlantsSection(state, body, *topic, width);

  const std::vector<std::string> related = Words(topic->related);
  if (!related.empty()) {
    Label(body, "Související", kMuted, &lvgl_font_google_sans_flex_22, width);
    for (const std::string& id : related) {
      const Topic* other = firstaid::FindTopic(id.c_str());
      if (other == nullptr) {
        continue;
      }
      lv_obj_t* button = Button(body, other->title, kChip, TopicButtonClicked, state, width, 68);
      lv_obj_set_user_data(button,
          reinterpret_cast<void*>(static_cast<intptr_t>(other - &firstaid::TopicAt(0))));
    }
  }
}

// ── Cabinet ─────────────────────────────────────────────────────────────────

void QuantityClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  lv_obj_t* button = lv_event_get_current_target_obj(event);
  const auto code = reinterpret_cast<intptr_t>(lv_obj_get_user_data(button));
  const size_t index = static_cast<size_t>(code >> 1);
  if (index >= state->cabinet.items().size()) {
    return;
  }
  CabinetItem& item = state->cabinet.items()[index];
  item.quantity = std::max(0.0f, item.quantity + ((code & 1) ? 1.0f : -1.0f));
  state->cabinet.Save();
  Haptic();
  lv_obj_t* label = lv_obj_get_child(lv_obj_get_parent(button), 1);
  if (label != nullptr) {
    lv_label_set_text(label, Num(item.quantity).c_str());
  }
}

void EditItemClicked(lv_event_t* event);

void ShowCabinet(FirstAidState* state) {
  lv_obj_t* body = nullptr;
  PushPage(state, "Lékárnička", &body);
  const int width = state->width - 2 * kSide;
  Button(body, "+ Přidat lék nebo pomůcku", 0x0A84FF, [](lv_event_t* event) {
    ShowAddItem(static_cast<FirstAidState*>(lv_event_get_user_data(event)));
  }, state, width, 76);
  auto& items = state->cabinet.items();
  if (items.empty()) {
    Label(body, "Lékárnička je prázdná. Přidej, co doma máš – průvodce pak u každého problému "
        "ukáže, čím ho léčit, kolik ti zbývá a jakou dávku vzít.", kMuted,
        &lvgl_font_google_sans_flex_24, width);
  }
  for (size_t i = 0; i < items.size(); ++i) {
    const CabinetItem& item = items[i];
    const Medicine* medicine = firstaid::FindMedicine(item.medicine.c_str());
    const int expiry = firstaid::ExpiryState(item, state->year, state->month);
    lv_obj_t* card = Card(body, width, kCard, expiry == 2 ? kRed : expiry == 1 ? kAmber : 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(card, EditItemClicked, LV_EVENT_CLICKED, state);
    Label(card, item.name.c_str(), kText, &lvgl_font_google_sans_flex_28, width - 36);
    std::string detail = firstaid::FormName(item.form);
    if (medicine != nullptr) detail = std::string(medicine->name) + " · " + detail;
    if (item.strength_mg > 0) detail += " · " + Num(item.strength_mg) + " mg";
    if (item.expiry_year > 0) {
      detail += " · do " + std::to_string(item.expiry_month) + "/" + std::to_string(item.expiry_year);
    }
    Label(card, detail.c_str(), expiry == 2 ? kRed : kMuted, &lvgl_font_google_sans_flex_22,
        width - 36);
    lv_obj_t* row = lv_obj_create(card);
    MakePlain(row);
    lv_obj_set_size(row, width - 36, 64);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 18, LV_PART_MAIN);
    lv_obj_t* minus = Button(row, "–", kChip, QuantityClicked, state, 72, 60);
    lv_obj_set_user_data(minus, reinterpret_cast<void*>(static_cast<intptr_t>(i << 1)));
    Label(row, Num(item.quantity).c_str(), kText, &lvgl_font_google_sans_flex_32);
    lv_obj_t* plus = Button(row, "+", kChip, QuantityClicked, state, 72, 60);
    lv_obj_set_user_data(plus, reinterpret_cast<void*>(static_cast<intptr_t>((i << 1) | 1)));
    Label(row, "zbývá", kMuted, &lvgl_font_google_sans_flex_22);
  }
}

lv_obj_t* Field(FirstAidState* state, lv_obj_t* body, const char* caption, const char* value,
    bool numeric, int width) {
  Label(body, caption, kMuted, &lvgl_font_google_sans_flex_22, width);
  lv_obj_t* field = lv_textarea_create(body);
  lv_textarea_set_one_line(field, true);
  lv_textarea_set_text(field, value != nullptr ? value : "");
  lv_obj_set_size(field, width, 68);
  lv_obj_set_style_radius(field, 18, LV_PART_MAIN);
  lv_obj_set_style_text_font(field, &lvgl_font_google_sans_flex_28, LV_PART_MAIN);
  lv_obj_set_user_data(field, numeric ? reinterpret_cast<void*>(1) : nullptr);
  lv_obj_add_event_cb(field, FieldFocused, LV_EVENT_FOCUSED, state);
  return field;
}

void SaveItemClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  CabinetItem item;
  if (state->edit_index >= 0 && state->edit_index < static_cast<int>(state->cabinet.items().size())) {
    item = state->cabinet.items()[state->edit_index];
  }
  item.medicine = state->edit_medicine;
  item.form = state->edit_form;
  item.name = lv_textarea_get_text(state->edit_name);
  if (item.name.empty()) {
    const Medicine* medicine = firstaid::FindMedicine(item.medicine.c_str());
    item.name = medicine != nullptr ? medicine->name : "Položka";
  }
  item.strength_mg = ParseNumber(lv_textarea_get_text(state->edit_strength));
  item.quantity = ParseNumber(lv_textarea_get_text(state->edit_quantity));
  item.expiry_month = std::clamp(static_cast<int>(ParseNumber(lv_textarea_get_text(state->edit_month))), 0, 12);
  item.expiry_year = static_cast<int>(ParseNumber(lv_textarea_get_text(state->edit_year)));
  if (item.expiry_year > 0 && item.expiry_year < 100) item.expiry_year += 2000;
  item.note = lv_textarea_get_text(state->edit_note);
  if (state->edit_index >= 0 && state->edit_index < static_cast<int>(state->cabinet.items().size())) {
    state->cabinet.items()[state->edit_index] = item;
  } else {
    state->cabinet.items().push_back(item);
  }
  state->cabinet.Save();
  PlayUiHapticEffect(HapticEffect::kSuccess);
  // Back to a fresh cabinet list.
  while (!state->pages.empty()) {
    PopPage(state);
  }
  ShowCabinet(state);
}

void DeleteItemClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  auto& items = state->cabinet.items();
  if (state->edit_index >= 0 && state->edit_index < static_cast<int>(items.size())) {
    items.erase(items.begin() + state->edit_index);
    state->cabinet.Save();
  }
  while (!state->pages.empty()) {
    PopPage(state);
  }
  ShowCabinet(state);
}

void ShowItemEditor(FirstAidState* state, int index) {
  state->edit_index = index;
  CabinetItem item;
  if (index >= 0) {
    item = state->cabinet.items()[index];
  } else {
    const Medicine* medicine = firstaid::FindMedicine(state->edit_medicine.c_str());
    item.medicine = state->edit_medicine;
    item.form = state->edit_form;
    if (medicine != nullptr) {
      item.name = medicine->name;
      item.strength_mg = medicine->usual_strength_mg;
      item.form = medicine->usual_form;
    }
  }
  state->edit_medicine = item.medicine;
  state->edit_form = item.form;
  lv_obj_t* body = nullptr;
  PushPage(state, index >= 0 ? "Upravit" : "Přidat do lékárničky", &body);
  const int width = state->width - 2 * kSide;
  const Medicine* medicine = firstaid::FindMedicine(item.medicine.c_str());
  if (medicine != nullptr) {
    Label(body, (std::string(medicine->name) + " – " + medicine->brands).c_str(), kMuted,
        &lvgl_font_google_sans_flex_22, width);
  }
  state->edit_name = Field(state, body, "Název (jak je na krabičce)", item.name.c_str(), false, width);
  const bool dosed = item.form == Form::kTablet || item.form == Form::kCapsule ||
      item.form == Form::kSyrup || item.form == Form::kDrops || item.form == Form::kSuppository;
  state->edit_strength = Field(state, body,
      item.form == Form::kSyrup || item.form == Form::kDrops ? "Síla: mg v 1 ml" : "Síla: mg v 1 kusu",
      item.strength_mg > 0 ? Num(item.strength_mg).c_str() : "", true, width);
  if (!dosed) {
    lv_obj_add_flag(state->edit_strength, LV_OBJ_FLAG_HIDDEN);
  }
  state->edit_quantity = Field(state, body,
      item.form == Form::kSyrup || item.form == Form::kDrops ? "Zbývá (ml)" : "Zbývá (kusů)",
      item.quantity > 0 ? Num(item.quantity).c_str() : "", true, width);
  state->edit_month = Field(state, body, "Expirace – měsíc (1–12)",
      item.expiry_month > 0 ? std::to_string(item.expiry_month).c_str() : "", true, width);
  state->edit_year = Field(state, body, "Expirace – rok",
      item.expiry_year > 0 ? std::to_string(item.expiry_year).c_str() : "", true, width);
  state->edit_note = Field(state, body, "Poznámka", item.note.c_str(), false, width);
  Button(body, "Uložit", 0x0A84FF, SaveItemClicked, state, width, 76);
  if (index >= 0) {
    Button(body, "Smazat", kRedDark, DeleteItemClicked, state, width, 68);
  }
  KeyboardFor(state, state->edit_quantity, true);
}

void EditItemClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  lv_obj_t* card = lv_event_get_current_target_obj(event);
  ShowItemEditor(state, static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(card))));
}

// Add picker: medicines (code >= 0), supplies (code -2 - index), own (-1).
void PickClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  lv_obj_t* button = lv_event_get_current_target_obj(event);
  const auto code = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
  if (code >= 0) {
    const Medicine& medicine = firstaid::MedicineAt(static_cast<size_t>(code));
    state->edit_medicine = medicine.key;
    state->edit_form = medicine.usual_form;
  } else if (code == -1) {
    state->edit_medicine.clear();
    state->edit_form = Form::kOther;
  } else {
    const firstaid::Supply& supply = firstaid::SupplyAt(static_cast<size_t>(-2 - code));
    state->edit_medicine.clear();
    state->edit_form = supply.form;
  }
  Haptic();
  ShowItemEditor(state, -1);
  if (code <= -2) {
    lv_textarea_set_text(state->edit_name, firstaid::SupplyAt(static_cast<size_t>(-2 - code)).name);
  }
}

void ShowAddItem(FirstAidState* state) {
  lv_obj_t* body = nullptr;
  PushPage(state, "Co přidat?", &body);
  const int width = state->width - 2 * kSide;
  Label(body, "Léky", kMuted, &lvgl_font_google_sans_flex_22, width);
  for (size_t i = 0; i < firstaid::MedicineCount(); ++i) {
    const Medicine& medicine = firstaid::MedicineAt(i);
    lv_obj_t* card = Card(body, width, kCard);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(card, PickClicked, LV_EVENT_CLICKED, state);
    Label(card, medicine.name, kText, &lvgl_font_google_sans_flex_28, width - 36);
    Label(card, medicine.brands, kMuted, &lvgl_font_google_sans_flex_22, width - 36);
  }
  Label(body, "Obvazy a pomůcky", kMuted, &lvgl_font_google_sans_flex_22, width);
  for (size_t i = 0; i < firstaid::SupplyCount(); ++i) {
    lv_obj_t* button = Button(body, firstaid::SupplyAt(i).name, kChip, PickClicked, state, width, 68);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(-2 - static_cast<int>(i))));
  }
  lv_obj_t* own = Button(body, "Vlastní položka", 0x0A84FF, PickClicked, state, width, 72);
  lv_obj_set_user_data(own, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
}

// ── People ──────────────────────────────────────────────────────────────────

void ShowPeople(FirstAidState* state);

void SavePersonClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  Person person;
  person.name = lv_textarea_get_text(state->person_name);
  if (person.name.empty()) person.name = "Osoba";
  person.age_years = std::max(0.0f, ParseNumber(lv_textarea_get_text(state->person_age)));
  person.weight_kg = std::max(1.0f, ParseNumber(lv_textarea_get_text(state->person_weight)));
  person.height_cm = std::max(0.0f, ParseNumber(lv_textarea_get_text(state->person_height)));
  auto& people = state->cabinet.people();
  if (state->person_index >= 0 && state->person_index < static_cast<int>(people.size())) {
    people[state->person_index] = person;
    state->cabinet.set_selected_person(state->person_index);
  } else {
    people.push_back(person);
    state->cabinet.set_selected_person(static_cast<int>(people.size()) - 1);
  }
  state->cabinet.Save();
  PlayUiHapticEffect(HapticEffect::kSuccess);
  while (!state->pages.empty()) {
    PopPage(state);
  }
  ShowPeople(state);
}

void DeletePersonClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  auto& people = state->cabinet.people();
  if (people.size() > 1 && state->person_index >= 0 &&
      state->person_index < static_cast<int>(people.size())) {
    people.erase(people.begin() + state->person_index);
    state->cabinet.set_selected_person(0);
    state->cabinet.Save();
  }
  while (!state->pages.empty()) {
    PopPage(state);
  }
  ShowPeople(state);
}

void ShowPersonEditor(FirstAidState* state, int index) {
  state->person_index = index;
  Person person;
  if (index >= 0) person = state->cabinet.people()[index];
  else person.name.clear();
  lv_obj_t* body = nullptr;
  PushPage(state, index >= 0 ? "Upravit osobu" : "Nová osoba", &body);
  const int width = state->width - 2 * kSide;
  state->person_name = Field(state, body, "Jméno", person.name.c_str(), false, width);
  state->person_age = Field(state, body, "Věk (roky, kojenec např. 0,5)", Num(person.age_years).c_str(), true, width);
  state->person_weight = Field(state, body, "Váha (kg)", Num(person.weight_kg).c_str(), true, width);
  state->person_height = Field(state, body, "Výška (cm)", Num(person.height_cm).c_str(), true, width);
  if (index >= 0) {
    char info[96];
    std::snprintf(info, sizeof(info), "BMI %.1f · povrch těla %.2f m²",
        firstaid::BodyMassIndex(person), firstaid::BodySurfaceArea(person));
    Label(body, info, kMuted, &lvgl_font_google_sans_flex_22, width);
  }
  Button(body, "Uložit", 0x0A84FF, SavePersonClicked, state, width, 76);
  if (index >= 0 && state->cabinet.people().size() > 1) {
    Button(body, "Smazat", kRedDark, DeletePersonClicked, state, width, 68);
  }
}

void PersonRowClicked(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  lv_obj_t* card = lv_event_get_current_target_obj(event);
  ShowPersonEditor(state, static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(card))));
}

void ShowPeople(FirstAidState* state) {
  lv_obj_t* body = nullptr;
  PushPage(state, "Pro koho", &body);
  const int width = state->width - 2 * kSide;
  Label(body, "Dávky léků se počítají podle věku a váhy vybrané osoby.", kMuted,
      &lvgl_font_google_sans_flex_22, width);
  const auto& people = state->cabinet.people();
  for (size_t i = 0; i < people.size(); ++i) {
    const bool selected = static_cast<int>(i) == state->cabinet.selected_person();
    lv_obj_t* card = Card(body, width, selected ? 0x0A3D7A : kCard);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(card, PersonRowClicked, LV_EVENT_CLICKED, state);
    Label(card, people[i].name.c_str(), kText, &lvgl_font_google_sans_flex_28, width - 36);
    Label(card, (Num(people[i].age_years) + " let · " + Num(people[i].weight_kg) + " kg · " +
                 Num(people[i].height_cm) + " cm").c_str(),
        kMuted, &lvgl_font_google_sans_flex_22, width - 36);
  }
  Button(body, "+ Přidat osobu", 0x0A84FF, [](lv_event_t* event) {
    ShowPersonEditor(static_cast<FirstAidState*>(lv_event_get_user_data(event)), -1);
  }, state, width, 76);
}

// ── Home ────────────────────────────────────────────────────────────────────

void RefreshHomeButtons(FirstAidState* state) {
  if (state->cabinet_button_label != nullptr) {
    int expired = 0;
    for (const CabinetItem& item : state->cabinet.items()) {
      expired += firstaid::ExpiryState(item, state->year, state->month) == 2;
    }
    std::string text = "Lékárnička · " + std::to_string(state->cabinet.items().size());
    if (expired > 0) text += " · " + std::to_string(expired) + " prošlé";
    lv_label_set_text(state->cabinet_button_label, text.c_str());
  }
  if (state->person_button_label != nullptr) {
    lv_label_set_text(state->person_button_label,
        ("Pro: " + PersonSummary(state->cabinet.Current())).c_str());
  }
}

void AddTopicRow(FirstAidState* state, lv_obj_t* list, size_t index, int width) {
  const Topic& topic = firstaid::TopicAt(index);
  lv_obj_t* row = lv_button_create(list);
  lv_obj_set_size(row, width, 76);
  lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(row, lv_color_hex(kCard), LV_PART_MAIN);
  lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
  lv_obj_add_event_cb(row, TopicButtonClicked, LV_EVENT_CLICKED, state);
  lv_obj_t* dot = lv_obj_create(row);
  MakePlain(dot);
  lv_obj_set_size(dot, 16, 16);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(dot, lv_color_hex(UrgencyColor(topic.urgency)), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(dot, LV_ALIGN_LEFT_MID, 4, 0);
  lv_obj_t* title = Label(row, topic.title, kText, &lvgl_font_google_sans_flex_28, width - 80);
  lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
  lv_obj_set_height(title, 36);
  lv_obj_align(title, LV_ALIGN_LEFT_MID, 34, 0);
}

void RenderHome(FirstAidState* state) {
  lv_obj_t* list = state->home_list;
  lv_obj_clean(list);
  const int width = state->width - 2 * kSide;
  const char* query = lv_textarea_get_text(state->search);
  if (query != nullptr && query[0] != '\0') {
    const std::vector<size_t> found = firstaid::SearchTopics(query, 30);
    if (found.empty()) {
      Label(list, "Nic nenalezeno. Zkus jiné slovo (např. krvácení, horečka, popálenina, "
          "kašel, vosa, klíště).", kMuted, &lvgl_font_google_sans_flex_24, width);
    }
    for (size_t index : found) {
      AddTopicRow(state, list, index, width);
    }
    return;
  }
  for (int category = 0; category < static_cast<int>(Category::kCount); ++category) {
    Label(list, firstaid::CategoryName(static_cast<Category>(category)), kMuted,
        &lvgl_font_google_sans_flex_22, width);
    for (size_t i = 0; i < firstaid::TopicCount(); ++i) {
      if (static_cast<int>(firstaid::TopicAt(i).category) == category) {
        AddTopicRow(state, list, i, width);
      }
    }
  }
}

void SearchTimer(lv_timer_t* timer) {
  lv_timer_pause(timer);
  RenderHome(static_cast<FirstAidState*>(lv_timer_get_user_data(timer)));
}

void SearchChanged(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  if (lv_event_get_code(event) == LV_EVENT_FOCUSED) {
    KeyboardFor(state, state->search, false);
    return;
  }
  lv_timer_reset(state->search_timer);
  lv_timer_resume(state->search_timer);
}

void ShowPriorities(FirstAidState* state) {
  lv_obj_t* body = nullptr;
  PushPage(state, "Co dřív", &body);
  const int width = state->width - 2 * kSide;
  lv_obj_t* card = Card(body, width, kRedDark, kRed);
  Label(card, firstaid::kPriorities, kText, &lvgl_font_google_sans_flex_28, width - 40);
  for (const char* id : {"bleeding", "choking", "cpr", "unconscious", "shock", "hypothermia"}) {
    const Topic* topic = firstaid::FindTopic(id);
    if (topic == nullptr) {
      continue;
    }
    lv_obj_t* button = Button(body, topic->title, topic->urgency == Urgency::kEmergency ? kRed : kChip,
        TopicButtonClicked, state, width, 76);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(
        topic - &firstaid::TopicAt(0))));
  }
  Label(body, "Když je to možné, dostaň zraněného k lékařské péči co nejdřív. Do té doby jsi "
      "jeho pomoc ty: drž ho v teple, hlídej dech a nenechávej ho samotného.", kBody,
      &lvgl_font_google_sans_flex_24, width);
}

void ViewDeleted(lv_event_t* event) {
  auto* state = static_cast<FirstAidState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  if (state->search_timer != nullptr) {
    lv_timer_delete(state->search_timer);
  }
  state->nature.Close();
  delete state;
}

}  // namespace

lv_obj_t* CreateFirstAidView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config) {
  (void)app_entry;
  if (parent == nullptr || config.width <= 0 || config.height <= 0) {
    return nullptr;
  }
  lv_obj_t* container = lv_obj_create(parent);
  MakePlain(container);
  lv_obj_set_size(container, config.width, config.height);
  lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_bg_color(container, lv_color_hex(theme::ActiveThemeColors().surface), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container, LV_OPA_COVER, LV_PART_MAIN);

  auto* state = new (std::nothrow) FirstAidState();
  if (state == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  state->root = container;
  state->width = config.width;
  state->height = config.height;
  state->storage = config.storage;
  state->rtc = config.rtc;
  lv_obj_add_event_cb(container, ViewDeleted, LV_EVENT_DELETE, state);
  hal::RtcStatus now;
  if (state->rtc != nullptr && state->rtc->ReadRtcStatus(&now) && now.ready && now.year >= 2024) {
    state->year = now.year;
    state->month = now.month;
  }
  state->cabinet.Load();

  const int width = config.width - 2 * kSide;
  lv_obj_t* title = Label(container, "First Aid", kText, &lvgl_font_google_sans_flex_36);
  lv_obj_set_pos(title, kSide, kTitleTop);
  lv_obj_t* call = Button(container, "Co dřív", kRed, [](lv_event_t* event) {
    ShowPriorities(static_cast<FirstAidState*>(lv_event_get_user_data(event)));
  }, state, LV_SIZE_CONTENT, 60);
  lv_obj_align(call, LV_ALIGN_TOP_RIGHT, -kSide, kTitleTop - 6);

  state->search = lv_textarea_create(container);
  lv_textarea_set_one_line(state->search, true);
  lv_textarea_set_placeholder_text(state->search, "Co se stalo? kašel, krvácení, vosa...");
  lv_obj_set_size(state->search, width, 72);
  lv_obj_set_pos(state->search, kSide, kTitleTop + 62);
  lv_obj_set_style_radius(state->search, 22, LV_PART_MAIN);
  lv_obj_set_style_text_font(state->search, &lvgl_font_google_sans_flex_28, LV_PART_MAIN);
  lv_obj_add_event_cb(state->search, SearchChanged, LV_EVENT_VALUE_CHANGED, state);
  lv_obj_add_event_cb(state->search, SearchChanged, LV_EVENT_FOCUSED, state);

  const int buttons_top = kTitleTop + 62 + 84;
  const int half = (width - 12) / 2;
  lv_obj_t* cabinet = Button(container, "Lékárnička", 0x2E7D32, [](lv_event_t* event) {
    ShowCabinet(static_cast<FirstAidState*>(lv_event_get_user_data(event)));
  }, state, half, 72);
  lv_obj_set_pos(cabinet, kSide, buttons_top);
  state->cabinet_button_label = lv_obj_get_child(cabinet, 0);
  lv_label_set_long_mode(state->cabinet_button_label, LV_LABEL_LONG_DOT);
  lv_obj_set_width(state->cabinet_button_label, half - 30);
  lv_obj_t* person = Button(container, "Pro: Já", kChip, [](lv_event_t* event) {
    ShowPeople(static_cast<FirstAidState*>(lv_event_get_user_data(event)));
  }, state, half, 72);
  lv_obj_set_pos(person, kSide + half + 12, buttons_top);
  state->person_button_label = lv_obj_get_child(person, 0);
  lv_label_set_long_mode(state->person_button_label, LV_LABEL_LONG_DOT);
  lv_obj_set_width(state->person_button_label, half - 30);

  const int list_top = buttons_top + 86;
  state->home_list = lv_obj_create(container);
  MakePlain(state->home_list);
  lv_obj_add_flag(state->home_list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(state->home_list, LV_DIR_VER);
  lv_obj_set_size(state->home_list, config.width, config.height - list_top);
  lv_obj_set_pos(state->home_list, 0, list_top);
  lv_obj_set_flex_flow(state->home_list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(state->home_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(state->home_list, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(state->home_list, 60, LV_PART_MAIN);
  lv_obj_add_event_cb(state->home_list, [](lv_event_t* event) {
    auto* s = static_cast<FirstAidState*>(lv_event_get_user_data(event));
    if (s->keyboard != nullptr) HideSharedKeyboard(s->keyboard);
  }, LV_EVENT_PRESSED, state);

  SharedKeyboardConfig keyboard_config;
  keyboard_config.width = config.width;
  keyboard_config.height = config.height / 3;
  state->keyboard = CreateSharedKeyboard(container, keyboard_config);
  if (state->keyboard != nullptr) {
    AttachSharedKeyboardToTextArea(state->keyboard, state->search, nullptr);
    HideSharedKeyboard(state->keyboard);
  }
  state->search_timer = lv_timer_create(SearchTimer, kSearchPauseMs, state);
  lv_timer_pause(state->search_timer);

  RefreshHomeButtons(state);
  RenderHome(state);
  RegisterConditionalBackNavigationHandler(container, [state]() {
    if (!state->pages.empty()) {
      PopPage(state);
      return true;
    }
    return false;
  });
  return container;
}

}  // namespace lilygo_box::ui
