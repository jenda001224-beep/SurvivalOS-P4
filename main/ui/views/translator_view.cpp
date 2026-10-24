/*
 * @Description: SurvivalOS offline Translator: dictionary, word-by-word
 *               sentences, phrasebook and speech in every bundled language
 * @License: GPL 3.0
 *
 * The dictionary comes from the English Wiktionary (see
 * tools/prepare_translator.py): every word links to "concepts" (an English
 * word in one sense) and every concept to its words in each language, so any
 * two languages translate through them. Sentences are translated word by word
 * with the longest phrase the dictionary knows taken first; that is a
 * dictionary's honest best, not machine translation, and the page says so.
 *
 * Words in scripts the built-in Latin fonts cannot draw are drawn from Go Noto
 * TTF fonts on the card; speech is eSpeak NG. Both arrive inside the bundle and
 * are unpacked to /sdcard/translator the first time.
 */
#include "ui/views/translator_view.h"

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>

#include "app/edition.h"
#include "base/logger.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/providers/audio_provider.h"
#include "hal/providers/storage_provider.h"
#include "nvs.h"
#include "survival/speech.h"
#include "survival/translator_pack.h"
#include "ui/haptic_feedback.h"
#include "ui/input/back_navigation_controller.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"
#include "ui/widgets/shared_keyboard.h"

namespace lilygo_box::ui {
namespace {

using survival::TranslatorPack;
using survival::TranslatorRecord;

constexpr const char* kTranslatorDirectory = "translator";
constexpr const char* kSpeechDirectory = "espeak-ng-data";
constexpr const char* kFontDirectory = "fonts";
constexpr const char* kMainFont = "GoNotoKurrent-Regular.ttf";
constexpr const char* kCjkFont = "GoNotoCJKCore.ttf";
constexpr const char* kSpeechFile = ".speak.wav";
constexpr const char* kNvsNamespace = "svos";
constexpr const char* kSourceKey = "tr_src";
constexpr const char* kTargetKey = "tr_dst";

constexpr int kSidePadding = 18;
constexpr int kTitleTop = 58;
constexpr int kBarTop = 112;
constexpr int kBarHeight = 72;
constexpr int kInputTop = kBarTop + kBarHeight + 14;
constexpr int kInputHeight = 76;
constexpr int kResultsTop = kInputTop + kInputHeight + 14;
constexpr uint32_t kTypingPauseMs = 450;
constexpr uint32_t kPollMs = 100;
constexpr int kRecordCapacity = 24;
constexpr int kMaxTranslations = 8;
constexpr int kMaxWords = 24;
constexpr int kMaxPhraseWords = 4;
constexpr int kPickerRows = 120;
constexpr int kPhraseRows = 80;
constexpr int kBigFontPx = 40;
constexpr int kSmallFontPx = 28;
constexpr uint32_t kWorkerStack = 6144;

constexpr uint32_t kAccent = 0x0A84FF;
constexpr uint32_t kMuted = 0x8E8E93;
constexpr uint32_t kCard = 0x1C1C1E;
constexpr uint32_t kChip = 0x2C2C2E;

enum class Job : int { kNone = 0, kExtract, kSpeak, kStop };
enum class JobState : int { kIdle = 0, kRunning, kDone, kFailed };

struct Translation {
  char word[124] = {};
  char roman[124] = {};
};

struct TranslatorViewState {
  hal::StorageProvider* storage = nullptr;
  hal::AudioProvider* audio = nullptr;
  int width = 0;
  int height = 0;

  lv_obj_t* root = nullptr;
  lv_obj_t* source_button = nullptr;
  lv_obj_t* source_label = nullptr;
  lv_obj_t* target_button = nullptr;
  lv_obj_t* target_label = nullptr;
  lv_obj_t* input = nullptr;
  lv_obj_t* results = nullptr;
  lv_obj_t* status = nullptr;
  lv_obj_t* keyboard = nullptr;
  lv_obj_t* overlay = nullptr;
  lv_obj_t* overlay_list = nullptr;
  lv_obj_t* overlay_search = nullptr;
  bool picking_target = false;
  int phrase_topic = -1;
  lv_timer_t* typing_timer = nullptr;
  lv_timer_t* poll_timer = nullptr;

  TranslatorPack pack;
  bool loaded = false;
  char pack_path[160] = {};
  char base_path[64] = {};
  int source = -1;
  int target = -1;
  TranslatorRecord* records = nullptr;
  TranslatorRecord* scratch = nullptr;

  lv_font_t* big_font = nullptr;
  lv_font_t* small_font = nullptr;
  lv_font_t* big_cjk = nullptr;
  lv_font_t* small_cjk = nullptr;

  // Worker: unpacks the bundle's files and speaks. Shared fields are only
  // written while job_state is kRunning (worker) or not (UI).
  TaskHandle_t worker = nullptr;
  SemaphoreHandle_t wake = nullptr;
  SemaphoreHandle_t worker_done = nullptr;
  std::atomic<int> job{static_cast<int>(Job::kNone)};
  std::atomic<int> job_state{static_cast<int>(JobState::kIdle)};
  bool resources_ready = false;
  bool fonts_tried = false;
  char speak_text[400] = {};
  char speak_voice[16] = {};
  survival::Speech speech;
};

// ── Small helpers ───────────────────────────────────────────────────────────

const theme::ThemeColors& Colors() { return theme::ActiveThemeColors(); }

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
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  }
  return label;
}

lv_obj_t* WrapLabel(lv_obj_t* parent, const char* text, uint32_t color,
    const lv_font_t* font, int width) {
  lv_obj_t* label = CreateLabel(parent, text, color, font);
  if (label != nullptr) {
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
  }
  return label;
}

bool IsAscii(const char* text) {
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p; ++p) {
    if (*p >= 0x80) {
      return false;
    }
  }
  return true;
}

// Word font: the card's TTF when it is loaded, else the Latin UI font (which
// still draws Latin, Czech and the romanisations).
const lv_font_t* WordFont(const TranslatorViewState* state, bool big) {
  const lv_font_t* ttf = big ? state->big_font : state->small_font;
  if (ttf != nullptr) {
    return ttf;
  }
  return big ? &lvgl_font_google_sans_flex_36 : &lvgl_font_google_sans_flex_28;
}

bool LoadText(const char* key, char* out, size_t size) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) {
    return false;
  }
  size_t length = size;
  const bool ok = nvs_get_str(handle, key, out, &length) == ESP_OK;
  nvs_close(handle);
  return ok;
}

void SaveText(const char* key, const char* value) {
  nvs_handle_t handle = 0;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
    return;
  }
  if (nvs_set_str(handle, key, value) == ESP_OK) {
    nvs_commit(handle);
  }
  nvs_close(handle);
}

void SetStatus(TranslatorViewState* state, const char* text) {
  if (state->status == nullptr) {
    return;
  }
  lv_label_set_text(state->status, text != nullptr ? text : "");
  if (text == nullptr || text[0] == '\0') {
    lv_obj_add_flag(state->status, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_remove_flag(state->status, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(state->status);
  }
}

// ── Worker ──────────────────────────────────────────────────────────────────

void WorkerTask(void* argument) {
  auto* state = static_cast<TranslatorViewState*>(argument);
  char path[200] = {};
  while (true) {
    xSemaphoreTake(state->wake, portMAX_DELAY);
    const Job job = static_cast<Job>(state->job.load());
    if (job == Job::kStop) {
      break;
    }
    bool ok = false;
    if (job == Job::kExtract || job == Job::kSpeak) {
      std::snprintf(path, sizeof(path), "%s/%s", state->base_path, kTranslatorDirectory);
      if (!state->resources_ready) {
        // A second reader of the same bundle, so the UI's stays untouched.
        TranslatorPack pack;
        state->resources_ready = pack.Open(state->pack_path) && pack.ExtractResources(path);
      }
      ok = state->resources_ready;
    }
    if (job == Job::kSpeak && ok) {
      char data[200] = {};
      std::snprintf(data, sizeof(data), "%s/%s", path, kSpeechDirectory);
      char wav[200] = {};
      std::snprintf(wav, sizeof(wav), "%s/%s", path, kSpeechFile);
      unsigned int duration_ms = 0;
      ok = state->speech.Open(data) &&
          state->speech.SynthesizeToFile(state->speak_text, state->speak_voice, wav,
              &duration_ms);
      if (ok && state->audio != nullptr) {
        ok = state->audio->StartAudioFile(wav, duration_ms);
      }
    }
    state->job_state.store(static_cast<int>(ok ? JobState::kDone : JobState::kFailed));
  }
  state->speech.Close();
  xSemaphoreGive(state->worker_done);
  vTaskDelete(nullptr);
}

bool StartJob(TranslatorViewState* state, Job job) {
  if (state->worker == nullptr ||
      state->job_state.load() == static_cast<int>(JobState::kRunning)) {
    return false;
  }
  state->job.store(static_cast<int>(job));
  state->job_state.store(static_cast<int>(JobState::kRunning));
  xSemaphoreGive(state->wake);
  return true;
}

void LoadFonts(TranslatorViewState* state) {
  if (state->fonts_tried) {
    return;
  }
  state->fonts_tried = true;
  char path[220] = {};
  struct stat info = {};
  std::snprintf(path, sizeof(path), "%s/%s/%s/%s", state->base_path, kTranslatorDirectory,
      kFontDirectory, kMainFont);
  if (stat(path, &info) != 0) {
    return;
  }
  char lv_path[230] = {};
  std::snprintf(lv_path, sizeof(lv_path), "S:%s", path);
  state->big_font = lv_tiny_ttf_create_file_ex(lv_path, kBigFontPx, LV_FONT_KERNING_NONE, 192);
  state->small_font = lv_tiny_ttf_create_file_ex(lv_path, kSmallFontPx, LV_FONT_KERNING_NONE, 256);
  std::snprintf(path, sizeof(path), "%s/%s/%s/%s", state->base_path, kTranslatorDirectory,
      kFontDirectory, kCjkFont);
  if (stat(path, &info) == 0) {
    std::snprintf(lv_path, sizeof(lv_path), "S:%s", path);
    state->big_cjk = lv_tiny_ttf_create_file_ex(lv_path, kBigFontPx, LV_FONT_KERNING_NONE, 128);
    state->small_cjk = lv_tiny_ttf_create_file_ex(lv_path, kSmallFontPx, LV_FONT_KERNING_NONE, 192);
  }
  if (state->big_font != nullptr) {
    state->big_font->fallback = state->big_cjk;
  }
  if (state->small_font != nullptr) {
    state->small_font->fallback = state->small_cjk;
  }
}

// ── Translation ─────────────────────────────────────────────────────────────

bool AddTranslation(Translation* out, int* count, int capacity, const char* word,
    const char* roman) {
  if (*count >= capacity || word == nullptr || word[0] == '\0') {
    return false;
  }
  for (int i = 0; i < *count; ++i) {
    if (std::strcmp(out[i].word, word) == 0) {
      return false;
    }
  }
  std::snprintf(out[*count].word, sizeof(out[*count].word), "%s", word);
  std::snprintf(out[*count].roman, sizeof(out[*count].roman), "%s",
      roman != nullptr ? roman : "");
  ++*count;
  return true;
}

// "to run (fast), quickly" -> "run": the head of an English gloss, used to
// look a meaning up as an English word.
bool GlossHead(const char* gloss, char* out, size_t size) {
  char text[128] = {};
  size_t length = 0;
  int depth = 0;
  for (const char* p = gloss; *p != '\0' && length + 1 < sizeof(text); ++p) {
    if (*p == '(') {
      ++depth;
    } else if (*p == ')') {
      depth = std::max(0, depth - 1);
    } else if (depth == 0) {
      if (*p == ',' || *p == ';' || *p == ':') {
        break;
      }
      text[length++] = *p;
    }
  }
  text[length] = '\0';
  char* start = text;
  while (*start == ' ') ++start;
  for (const char* article : {"to ", "a ", "an ", "the "}) {
    const size_t n = std::strlen(article);
    if (strncasecmp(start, article, n) == 0) {
      start += n;
      break;
    }
  }
  size_t end = std::strlen(start);
  while (end > 0 && (start[end - 1] == ' ' || start[end - 1] == '.')) --end;
  start[end] = '\0';
  int words = start[0] != '\0' ? 1 : 0;
  for (const char* p = start; *p; ++p) words += *p == ' ';
  if (words == 0 || words > 3) {
    return false;
  }
  std::snprintf(out, size, "%s", start);
  return true;
}

// Words of the target language for a concept.
void ConceptTranslations(TranslatorViewState* state, uint32_t concept_id, Translation* out,
    int* count, int capacity) {
  uint32_t offsets[4] = {};
  const int found = state->pack.ConceptWords(state->target, concept_id, offsets, 4);
  for (int i = 0; i < found && *count < capacity; ++i) {
    TranslatorRecord& word = *state->scratch;
    if (state->pack.ReadRecord(state->target, offsets[i], &word)) {
      AddTranslation(out, count, capacity, word.word, word.roman);
    }
  }
}

int TranslateRecord(TranslatorViewState* state, const TranslatorRecord& record,
    Translation* out, int capacity, bool allow_lemma = true) {
  int count = 0;
  if (state->target == state->source) {
    AddTranslation(out, &count, capacity, record.word, record.roman);
    return count;
  }
  for (int c = 0; c < record.concept_count && count < capacity && c < 10; ++c) {
    ConceptTranslations(state, record.concepts[c], out, &count, capacity);
  }
  if (count == 0 && allow_lemma && record.lemma[0] != '\0') {
    TranslatorRecord* lemma = static_cast<TranslatorRecord*>(
        heap_caps_malloc(sizeof(TranslatorRecord), MALLOC_CAP_SPIRAM));
    if (lemma != nullptr) {
      new (lemma) TranslatorRecord();
      if (state->pack.Find(state->source, record.lemma, false, lemma, 1) > 0) {
        count = TranslateRecord(state, *lemma, out, capacity, false);
      }
      heap_caps_free(lemma);
    }
  }
  if (count > 0 || record.glosses[0] == '\0') {
    return count;
  }
  // No translation table lists this word: go through its English meanings.
  // (Static: the LVGL task's stack is only 16 kB; this runs on it alone.)
  const int english = state->pack.FindLanguage("en");
  EXT_RAM_BSS_ATTR static char glosses[sizeof(record.glosses)];
  std::snprintf(glosses, sizeof(glosses), "%s", record.glosses);
  char* save = nullptr;
  int used = 0;
  for (char* gloss = strtok_r(glosses, "\x1f", &save); gloss != nullptr && used < 3 &&
       count < capacity; gloss = strtok_r(nullptr, "\x1f", &save), ++used) {
    char head[96] = {};
    if (!GlossHead(gloss, head, sizeof(head))) {
      continue;
    }
    if (state->target == english) {
      AddTranslation(out, &count, capacity, head, nullptr);
      continue;
    }
    TranslatorRecord* found = static_cast<TranslatorRecord*>(
        heap_caps_calloc(2, sizeof(TranslatorRecord), MALLOC_CAP_SPIRAM));
    if (found == nullptr) {
      break;
    }
    new (&found[0]) TranslatorRecord();
    new (&found[1]) TranslatorRecord();
    const int n = state->pack.Find(english, head, false, found, 2);
    for (int r = 0; r < n && count < capacity; ++r) {
      for (int c = 0; c < found[r].concept_count && c < 4 && count < capacity; ++c) {
        ConceptTranslations(state, found[r].concepts[c], out, &count, capacity);
      }
    }
    heap_caps_free(found);
  }
  return count;
}

// ── Results ─────────────────────────────────────────────────────────────────

void SpeakClicked(lv_event_t* event);

lv_obj_t* AddCard(TranslatorViewState* state) {
  lv_obj_t* card = lv_obj_create(state->results);
  MakePlain(card);
  lv_obj_set_width(card, state->width - 2 * kSidePadding);
  lv_obj_set_height(card, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(card, lv_color_hex(kCard), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(card, 22, LV_PART_MAIN);
  lv_obj_set_style_pad_all(card, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_row(card, 6, LV_PART_MAIN);
  lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
  return card;
}

// A speaker button that says `text` in `language` when tapped.
void AddSpeakButton(TranslatorViewState* state, lv_obj_t* parent, int language,
    const char* text) {
  if (language < 0 || state->pack.Language(language).voice[0] == '\0' ||
      state->audio == nullptr) {
    return;
  }
  lv_obj_t* button = lv_button_create(parent);
  lv_obj_add_flag(button, LV_OBJ_FLAG_FLOATING);
  lv_obj_set_size(button, 64, 64);
  lv_obj_align(button, LV_ALIGN_TOP_RIGHT, 0, 0);
  lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(kChip), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_t* icon = CreateLabel(button, icon::kVolumeUp, 0xFFFFFF,
      &lvgl_font_material_symbols_fill_32);
  lv_obj_center(icon);
  // The text rides along in the button's own label, hidden.
  lv_obj_t* payload = CreateLabel(button, text, 0, &lvgl_font_google_sans_flex_22);
  lv_obj_add_flag(payload, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<intptr_t>(language)));
  lv_obj_add_event_cb(button, SpeakClicked, LV_EVENT_CLICKED, state);
}

void ShowHint(TranslatorViewState* state, const char* text) {
  lv_obj_clean(state->results);
  lv_obj_t* label = WrapLabel(state->results, text, kMuted, &lvgl_font_google_sans_flex_24,
      state->width - 2 * kSidePadding);
  lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

void AddTranslationLines(TranslatorViewState* state, lv_obj_t* card, const Translation& item,
    bool big) {
  const int width = state->width - 2 * kSidePadding - 36 - (big ? 76 : 0);
  WrapLabel(card, item.word, Colors().on_surface, WordFont(state, big), width);
  if (item.roman[0] != '\0') {
    WrapLabel(card, item.roman, kMuted, &lvgl_font_google_sans_flex_24, width);
  }
}

void ShowWordResult(TranslatorViewState* state, int found) {
  lv_obj_clean(state->results);
  EXT_RAM_BSS_ATTR static Translation translations[kMaxTranslations];
  int count = 0;
  for (int r = 0; r < found && count < kMaxTranslations; ++r) {
    EXT_RAM_BSS_ATTR static Translation more[kMaxTranslations];
    const int n = TranslateRecord(state, state->records[r], more, kMaxTranslations);
    for (int i = 0; i < n; ++i) {
      AddTranslation(translations, &count, kMaxTranslations, more[i].word, more[i].roman);
    }
  }
  const int width = state->width - 2 * kSidePadding - 36;
  lv_obj_t* card = AddCard(state);
  if (count == 0) {
    WrapLabel(card, "No translation in this dictionary.", kMuted,
        &lvgl_font_google_sans_flex_24, width);
  } else {
    AddTranslationLines(state, card, translations[0], true);
    AddSpeakButton(state, card, state->target, translations[0].word);
    if (count > 1) {
      lv_obj_t* also = CreateLabel(card, "Also", kMuted, &lvgl_font_google_sans_flex_22);
      lv_obj_set_style_pad_top(also, 10, LV_PART_MAIN);
      for (int i = 1; i < count; ++i) {
        AddTranslationLines(state, card, translations[i], false);
      }
    }
  }

  // What the word means, in English, straight from the dictionary.
  for (int r = 0; r < found && r < 4; ++r) {
    const TranslatorRecord& record = state->records[r];
    if (record.glosses[0] == '\0' && record.lemma[0] == '\0') {
      continue;
    }
    lv_obj_t* meaning = AddCard(state);
    char heading[200] = {};
    const char* pos = survival::TranslatorPartOfSpeechName(record.part_of_speech);
    std::snprintf(heading, sizeof(heading), "%s%s%s%s", record.word, pos[0] ? "  · " : "", pos,
        record.roman[0] ? "" : "");
    WrapLabel(meaning, heading, Colors().on_surface, WordFont(state, false), width - 76);
    AddSpeakButton(state, meaning, state->source, record.word);
    if (record.roman[0] != '\0') {
      WrapLabel(meaning, record.roman, kMuted, &lvgl_font_google_sans_flex_22, width);
    }
    if (record.lemma[0] != '\0') {
      char lemma[160] = {};
      std::snprintf(lemma, sizeof(lemma), "Form of %s", record.lemma);
      WrapLabel(meaning, lemma, kAccent, &lvgl_font_google_sans_flex_22, width);
    }
    EXT_RAM_BSS_ATTR static char glosses[sizeof(record.glosses)];
    std::snprintf(glosses, sizeof(glosses), "%s", record.glosses);
    char* save = nullptr;
    int shown = 0;
    for (char* gloss = strtok_r(glosses, "\x1f", &save); gloss != nullptr && shown < 6;
         gloss = strtok_r(nullptr, "\x1f", &save), ++shown) {
      char line[300] = {};
      std::snprintf(line, sizeof(line), "%d. %s", shown + 1, gloss);
      WrapLabel(meaning, line, 0xD1D1D6, &lvgl_font_google_sans_flex_22, width);
    }
  }
  lv_obj_scroll_to_y(state->results, 0, LV_ANIM_OFF);
}

int SplitWords(char* text, char** words, int capacity) {
  int count = 0;
  char* p = text;
  while (*p != '\0' && count < capacity) {
    while (*p != '\0' && (std::strchr(" \t\n.,!?;:\"()[]", *p) != nullptr)) {
      *p++ = '\0';
    }
    if (*p == '\0') {
      break;
    }
    words[count++] = p;
    while (*p != '\0' && std::strchr(" \t\n.,!?;:\"()[]", *p) == nullptr) {
      ++p;
    }
  }
  return count;
}

void ShowSentenceResult(TranslatorViewState* state, char** words, int word_count) {
  lv_obj_clean(state->results);
  char sentence[600] = {};
  char roman[600] = {};
  size_t used = 0;
  size_t roman_used = 0;
  const int width = state->width - 2 * kSidePadding - 36;
  lv_obj_t* breakdown_card = nullptr;
  int i = 0;
  while (i < word_count) {
    int taken = 1;
    Translation best[2];
    int best_count = 0;
    // Longest run of words the dictionary knows as one entry.
    for (int n = std::min(kMaxPhraseWords, word_count - i); n >= 1; --n) {
      char phrase[200] = {};
      size_t length = 0;
      for (int k = 0; k < n; ++k) {
        length += std::snprintf(phrase + length, sizeof(phrase) - length, "%s%s",
            k ? " " : "", words[i + k]);
      }
      const int found = state->pack.Find(state->source, phrase, false, state->records, 4);
      if (found == 0) {
        continue;
      }
      for (int r = 0; r < found && best_count == 0; ++r) {
        best_count = TranslateRecord(state, state->records[r], best, 2);
      }
      if (best_count > 0 || n == 1) {
        taken = n;
        break;
      }
    }
    char source_text[200] = {};
    size_t length = 0;
    for (int k = 0; k < taken; ++k) {
      length += std::snprintf(source_text + length, sizeof(source_text) - length, "%s%s",
          k ? " " : "", words[i + k]);
    }
    const char* output = best_count > 0 ? best[0].word : source_text;
    used += std::snprintf(sentence + used, sizeof(sentence) - std::min(used, sizeof(sentence)),
        "%s%s", used ? " " : "", output);
    used = std::min(used, sizeof(sentence) - 1);
    const char* out_roman = best_count > 0 && best[0].roman[0] ? best[0].roman : output;
    roman_used += std::snprintf(roman + roman_used,
        sizeof(roman) - std::min(roman_used, sizeof(roman)), "%s%s", roman_used ? " " : "",
        out_roman);
    roman_used = std::min(roman_used, sizeof(roman) - 1);

    if (breakdown_card == nullptr) {
      // Placeholder: the sentence card goes first, so it is created below and
      // moved to the top.
      breakdown_card = AddCard(state);
      CreateLabel(breakdown_card, "Word by word", kMuted, &lvgl_font_google_sans_flex_22);
    }
    char line[400] = {};
    // The arrow only exists in the card's TTF font.
    std::snprintf(line, sizeof(line), state->small_font != nullptr ? "%s  →  %s%s%s%s"
        : "%s  =  %s%s%s%s", source_text,
        best_count > 0 ? best[0].word : "?", best_count > 0 && best[0].roman[0] ? "  (" : "",
        best_count > 0 && best[0].roman[0] ? best[0].roman : "",
        best_count > 0 && best[0].roman[0] ? ")" : "");
    WrapLabel(breakdown_card, line, best_count > 0 ? 0xD1D1D6 : kMuted, WordFont(state, false),
        width);
    i += taken;
  }
  lv_obj_t* card = AddCard(state);
  lv_obj_move_to_index(card, 0);
  Translation whole;
  std::snprintf(whole.word, sizeof(whole.word), "%s", sentence);
  if (std::strcmp(roman, sentence) != 0 && !IsAscii(sentence)) {
    std::snprintf(whole.roman, sizeof(whole.roman), "%s", roman);
  }
  AddTranslationLines(state, card, whole, true);
  AddSpeakButton(state, card, state->target, sentence);
  WrapLabel(card, "Translated word by word from the dictionary; check grammar and word order.",
      kMuted, &lvgl_font_google_sans_flex_22, width);
  lv_obj_scroll_to_y(state->results, 0, LV_ANIM_OFF);
}

void ShowSuggestions(TranslatorViewState* state, const char* text);

void RunTranslation(TranslatorViewState* state) {
  if (!state->loaded) {
    return;
  }
  const char* text = lv_textarea_get_text(state->input);
  char buffer[400] = {};
  std::snprintf(buffer, sizeof(buffer), "%s", text != nullptr ? text : "");
  char* words[kMaxWords] = {};
  char copy[400] = {};
  std::snprintf(copy, sizeof(copy), "%s", buffer);
  const int word_count = SplitWords(copy, words, kMaxWords);
  if (word_count == 0) {
    ShowHint(state, "Type a word or a short sentence.\n\nTap a language to change it; "
        "the button between them swaps them. Phrases has ready-made sentences.");
    return;
  }
  // The whole input first: "thank you", "dobrý den" are entries of their own.
  int found = state->pack.Find(state->source, buffer, false, state->records, kRecordCapacity);
  if (found == 0 && word_count == 1) {
    ShowSuggestions(state, words[0]);
    return;
  }
  if (found > 0) {
    ShowWordResult(state, found);
    return;
  }
  ShowSentenceResult(state, words, word_count);
}

void SuggestionClicked(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  lv_obj_t* label = lv_obj_get_child(lv_event_get_target_obj(event), 0);
  if (state == nullptr || label == nullptr) {
    return;
  }
  lv_textarea_set_text(state->input, lv_label_get_text(label));
  RunTranslation(state);
}

void ShowSuggestions(TranslatorViewState* state, const char* text) {
  lv_obj_clean(state->results);
  const int found = state->pack.Find(state->source, text, true, state->records, kRecordCapacity);
  if (found == 0) {
    ShowHint(state, "Not in this dictionary.\n\nTry the dictionary form of the word "
        "(infinitive, singular), or another spelling.");
    return;
  }
  CreateLabel(state->results, "Did you mean", kMuted, &lvgl_font_google_sans_flex_22);
  for (int i = 0; i < found; ++i) {
    lv_obj_t* row = lv_button_create(state->results);
    lv_obj_set_width(row, state->width - 2 * kSidePadding);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(kCard), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
    lv_obj_t* label = CreateLabel(row, state->records[i].word, Colors().on_surface,
        WordFont(state, false));
    (void)label;
    lv_obj_add_event_cb(row, SuggestionClicked, LV_EVENT_CLICKED, state);
  }
}

// ── Speech ──────────────────────────────────────────────────────────────────

void Speak(TranslatorViewState* state, int language, const char* text) {
  if (language < 0 || text == nullptr || text[0] == '\0') {
    return;
  }
  const char* voice = state->pack.Language(language).voice;
  if (voice[0] == '\0') {
    SetStatus(state, "No voice for this language");
    return;
  }
  if (state->job_state.load() == static_cast<int>(JobState::kRunning)) {
    return;
  }
  std::snprintf(state->speak_text, sizeof(state->speak_text), "%s", text);
  std::snprintf(state->speak_voice, sizeof(state->speak_voice), "%s", voice);
  if (StartJob(state, Job::kSpeak)) {
    PlayUiHapticEffect(HapticEffect::kSelection);
    SetStatus(state, state->resources_ready ? "Speaking..." :
        "Preparing speech (first time only)...");
  }
}

void SpeakClicked(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  lv_obj_t* button = lv_event_get_target_obj(event);
  if (state == nullptr || button == nullptr) {
    return;
  }
  const int language = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(button)));
  lv_obj_t* payload = lv_obj_get_child(button, 1);
  if (payload != nullptr) {
    Speak(state, language, lv_label_get_text(payload));
  }
}

void PollTimer(lv_timer_t* timer) {
  auto* state = static_cast<TranslatorViewState*>(lv_timer_get_user_data(timer));
  const JobState job_state = static_cast<JobState>(state->job_state.load());
  if (job_state == JobState::kDone || job_state == JobState::kFailed) {
    const Job job = static_cast<Job>(state->job.load());
    state->job_state.store(static_cast<int>(JobState::kIdle));
    if (job_state == JobState::kFailed) {
      SetStatus(state, job == Job::kSpeak ? "Could not speak this" :
          "Could not unpack the fonts and voices");
    } else {
      SetStatus(state, nullptr);
    }
    if (state->resources_ready && !state->fonts_tried) {
      LoadFonts(state);
      RunTranslation(state);
    }
  }
}

// ── Languages ───────────────────────────────────────────────────────────────

void RefreshLanguageButtons(TranslatorViewState* state) {
  if (!state->loaded) {
    return;
  }
  lv_label_set_text(state->source_label, state->pack.Language(state->source).name);
  lv_label_set_text(state->target_label, state->pack.Language(state->target).name);
}

void CloseOverlay(TranslatorViewState* state) {
  if (state->overlay == nullptr) {
    return;
  }
  lv_obj_delete(state->overlay);
  state->overlay = nullptr;
  state->overlay_list = nullptr;
  state->overlay_search = nullptr;
  if (state->keyboard != nullptr) {
    AttachSharedKeyboardToTextArea(state->keyboard, state->input, nullptr);
    HideSharedKeyboard(state->keyboard);
  }
}

void OverlayBackClicked(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  if (state != nullptr) {
    CloseOverlay(state);
  }
}

lv_obj_t* OpenOverlay(TranslatorViewState* state, const char* title) {
  CloseOverlay(state);
  lv_obj_t* overlay = lv_obj_create(state->root);
  MakePlain(overlay);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(overlay, state->width, state->height);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(Colors().surface), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_t* back = lv_button_create(overlay);
  lv_obj_set_size(back, 72, 72);
  lv_obj_set_pos(back, kSidePadding - 6, kTitleTop - 14);
  lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(back, lv_color_hex(kChip), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(back, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(back, OverlayBackClicked, LV_EVENT_CLICKED, state);
  lv_obj_t* arrow = CreateLabel(back, icon::kArrowBack, 0xFFFFFF,
      &lvgl_font_material_symbols_fill_32);
  lv_obj_center(arrow);
  lv_obj_t* heading = CreateLabel(overlay, title, Colors().on_surface,
      &lvgl_font_google_sans_flex_32);
  lv_obj_set_pos(heading, kSidePadding + 84, kTitleTop + 2);
  state->overlay = overlay;
  if (state->keyboard != nullptr) {
    lv_obj_move_foreground(state->keyboard);
  }
  return overlay;
}

lv_obj_t* AddOverlayList(TranslatorViewState* state, int top) {
  lv_obj_t* list = lv_obj_create(state->overlay);
  MakePlain(list);
  lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(list, LV_DIR_VER);
  lv_obj_set_size(list, state->width, state->height - top);
  lv_obj_set_pos(list, 0, top);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(list, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(list, 40, LV_PART_MAIN);
  state->overlay_list = list;
  return list;
}

void LanguageRowClicked(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  lv_obj_t* row = lv_event_get_target_obj(event);
  if (state == nullptr || row == nullptr) {
    return;
  }
  const int language = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
  if (state->picking_target) {
    state->target = language;
    SaveText(kTargetKey, state->pack.Language(language).code);
  } else {
    state->source = language;
    SaveText(kSourceKey, state->pack.Language(language).code);
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  CloseOverlay(state);
  RefreshLanguageButtons(state);
  RunTranslation(state);
}

bool ContainsFolded(const char* haystack, const char* needle) {
  if (needle[0] == '\0') {
    return true;
  }
  const size_t n = std::strlen(needle);
  for (const char* p = haystack; *p; ++p) {
    if (strncasecmp(p, needle, n) == 0) {
      return true;
    }
  }
  return false;
}

void FillLanguageList(TranslatorViewState* state) {
  lv_obj_t* list = state->overlay_list;
  if (list == nullptr) {
    return;
  }
  lv_obj_clean(list);
  const char* query = state->overlay_search != nullptr
      ? lv_textarea_get_text(state->overlay_search) : "";
  int shown = 0;
  for (int i = 0; i < state->pack.LanguageCount() && shown < kPickerRows; ++i) {
    const survival::TranslatorLanguage& language = state->pack.Language(i);
    if (!ContainsFolded(language.name, query) && strcasecmp(language.code, query) != 0) {
      continue;
    }
    lv_obj_t* row = lv_button_create(list);
    lv_obj_set_size(row, state->width - 2 * kSidePadding, 84);
    const bool current = i == (state->picking_target ? state->target : state->source);
    lv_obj_set_style_bg_color(row, lv_color_hex(current ? 0x0A3D7A : kCard), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(row, LanguageRowClicked, LV_EVENT_CLICKED, state);
    lv_obj_t* name = CreateLabel(row, language.name, Colors().on_surface,
        &lvgl_font_google_sans_flex_28);
    lv_obj_align(name, LV_ALIGN_LEFT_MID, 8, -12);
    char detail[96] = {};
    std::snprintf(detail, sizeof(detail), "%s · %u words", language.code,
        static_cast<unsigned>(language.entry_count));
    lv_obj_t* code = CreateLabel(row, detail, kMuted, &lvgl_font_google_sans_flex_22);
    lv_obj_align(code, LV_ALIGN_LEFT_MID, 8, 20);
    if (language.voice[0] != '\0') {
      lv_obj_t* voice = CreateLabel(row, icon::kVolumeUp, kMuted,
          &lvgl_font_material_symbols_fill_32);
      lv_obj_align(voice, LV_ALIGN_RIGHT_MID, -6, 0);
    }
    ++shown;
  }
  if (shown == 0) {
    CreateLabel(list, "No language matches.", kMuted, &lvgl_font_google_sans_flex_24);
  }
}

void LanguageSearchChanged(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  if (state != nullptr) {
    FillLanguageList(state);
  }
}

void OpenLanguagePicker(TranslatorViewState* state, bool target) {
  if (!state->loaded) {
    return;
  }
  state->picking_target = target;
  lv_obj_t* overlay = OpenOverlay(state, target ? "Translate to" : "Translate from");
  state->overlay_search = lv_textarea_create(overlay);
  lv_textarea_set_one_line(state->overlay_search, true);
  lv_textarea_set_placeholder_text(state->overlay_search, "Search languages");
  lv_obj_set_size(state->overlay_search, state->width - 2 * kSidePadding, 68);
  lv_obj_set_pos(state->overlay_search, kSidePadding, kBarTop + 8);
  lv_obj_set_style_radius(state->overlay_search, 20, LV_PART_MAIN);
  lv_obj_set_style_text_font(state->overlay_search, &lvgl_font_google_sans_flex_24, LV_PART_MAIN);
  lv_obj_add_event_cb(state->overlay_search, LanguageSearchChanged, LV_EVENT_VALUE_CHANGED,
      state);
  if (state->keyboard != nullptr) {
    AttachSharedKeyboardToTextArea(state->keyboard, state->overlay_search, nullptr);
    HideSharedKeyboard(state->keyboard);
  }
  AddOverlayList(state, kBarTop + 8 + 68 + 14);
  FillLanguageList(state);
}

void SourceClicked(lv_event_t* event) {
  OpenLanguagePicker(static_cast<TranslatorViewState*>(lv_event_get_user_data(event)), false);
}

void TargetClicked(lv_event_t* event) {
  OpenLanguagePicker(static_cast<TranslatorViewState*>(lv_event_get_user_data(event)), true);
}

void SwapClicked(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  if (state == nullptr || !state->loaded) {
    return;
  }
  std::swap(state->source, state->target);
  SaveText(kSourceKey, state->pack.Language(state->source).code);
  SaveText(kTargetKey, state->pack.Language(state->target).code);
  PlayUiHapticEffect(HapticEffect::kSelection);
  RefreshLanguageButtons(state);
  RunTranslation(state);
}

// ── Phrasebook ──────────────────────────────────────────────────────────────

void PhraseClicked(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  lv_obj_t* row = lv_event_get_target_obj(event);
  if (state == nullptr || row == nullptr) {
    return;
  }
  // Child 1 holds the translation.
  lv_obj_t* word = lv_obj_get_child(row, 1);
  if (word != nullptr) {
    Speak(state, state->target, lv_label_get_text(word));
  }
}

void FillPhrases(TranslatorViewState* state) {
  lv_obj_t* list = state->overlay_list;
  if (list == nullptr) {
    return;
  }
  lv_obj_clean(list);
  const int width = state->width - 2 * kSidePadding - 32;
  int shown = 0;
  for (int i = 0; i < state->pack.PhraseCount() && shown < kPhraseRows; ++i) {
    const survival::TranslatorPhrase& phrase = state->pack.Phrase(i);
    if (state->phrase_topic >= 0 && phrase.topic != state->phrase_topic) {
      continue;
    }
    Translation words[1];
    int count = 0;
    ConceptTranslations(state, phrase.concept_id, words, &count, 1);
    if (count == 0) {
      continue;
    }
    char english[160] = {};
    char sense[200] = {};
    state->pack.Concept(phrase.concept_id, english, sizeof(english), sense, sizeof(sense));
    lv_obj_t* row = lv_obj_create(list);
    MakePlain(row);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(row, state->width - 2 * kSidePadding);
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(row, lv_color_hex(kCard), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2C2C2E), LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 18, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 16, LV_PART_MAIN);
    lv_obj_set_style_pad_row(row, 4, LV_PART_MAIN);
    lv_obj_add_event_cb(row, PhraseClicked, LV_EVENT_CLICKED, state);
    lv_obj_t* source = WrapLabel(row, english, kMuted, &lvgl_font_google_sans_flex_24, width);
    lv_obj_remove_flag(source, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* target = WrapLabel(row, words[0].word, Colors().on_surface, WordFont(state, false),
        width);
    lv_obj_remove_flag(target, LV_OBJ_FLAG_CLICKABLE);
    if (words[0].roman[0] != '\0') {
      WrapLabel(row, words[0].roman, kMuted, &lvgl_font_google_sans_flex_22, width);
    }
    if (sense[0] != '\0') {
      WrapLabel(row, sense, 0x636366, &lvgl_font_google_sans_flex_22, width);
    }
    ++shown;
  }
  if (shown == 0) {
    WrapLabel(list, "No phrases in this language here yet.", kMuted,
        &lvgl_font_google_sans_flex_24, width);
  }
  lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
}

void TopicClicked(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  lv_obj_t* chip = lv_event_get_target_obj(event);
  if (state == nullptr || chip == nullptr) {
    return;
  }
  state->phrase_topic = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(chip)));
  lv_obj_t* row = lv_obj_get_parent(chip);
  for (uint32_t i = 0; i < lv_obj_get_child_count(row); ++i) {
    lv_obj_t* other = lv_obj_get_child(row, static_cast<int32_t>(i));
    const bool on = other == chip;
    lv_obj_set_style_bg_color(other, lv_color_hex(on ? kAccent : kChip), LV_PART_MAIN);
  }
  PlayUiHapticEffect(HapticEffect::kSelection);
  FillPhrases(state);
}

lv_obj_t* AddTopicChip(TranslatorViewState* state, lv_obj_t* row, const char* text, int topic) {
  lv_obj_t* chip = lv_button_create(row);
  lv_obj_set_height(chip, 52);
  lv_obj_set_width(chip, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(chip, 20, LV_PART_MAIN);
  lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(chip, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(chip, lv_color_hex(topic == state->phrase_topic ? kAccent : kChip),
      LV_PART_MAIN);
  lv_obj_set_user_data(chip, reinterpret_cast<void*>(static_cast<intptr_t>(topic)));
  lv_obj_add_event_cb(chip, TopicClicked, LV_EVENT_CLICKED, state);
  lv_obj_t* label = CreateLabel(chip, text, 0xFFFFFF, &lvgl_font_google_sans_flex_22);
  lv_obj_center(label);
  return chip;
}

void PhrasesClicked(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  if (state == nullptr || !state->loaded) {
    return;
  }
  if (state->keyboard != nullptr) {
    HideSharedKeyboard(state->keyboard);
  }
  char title[80] = {};
  std::snprintf(title, sizeof(title), "Phrases · %s", state->pack.Language(state->target).name);
  lv_obj_t* overlay = OpenOverlay(state, title);
  lv_obj_t* row = lv_obj_create(overlay);
  MakePlain(row);
  lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(row, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
  lv_obj_set_size(row, state->width, 60);
  lv_obj_set_pos(row, 0, kBarTop + 10);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_left(row, kSidePadding, LV_PART_MAIN);
  if (state->phrase_topic >= state->pack.TopicCount()) {
    state->phrase_topic = -1;
  }
  AddTopicChip(state, row, "All", -1);
  for (int i = 0; i < state->pack.TopicCount(); ++i) {
    AddTopicChip(state, row, state->pack.Topic(i), i);
  }
  AddOverlayList(state, kBarTop + 84);
  FillPhrases(state);
}

// ── Main page ───────────────────────────────────────────────────────────────

void TypingTimer(lv_timer_t* timer) {
  auto* state = static_cast<TranslatorViewState*>(lv_timer_get_user_data(timer));
  lv_timer_pause(timer);
  RunTranslation(state);
}

void InputChanged(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->typing_timer == nullptr) {
    return;
  }
  if (lv_event_get_code(event) == LV_EVENT_READY) {
    lv_timer_pause(state->typing_timer);
    if (state->keyboard != nullptr) {
      HideSharedKeyboard(state->keyboard);
    }
    RunTranslation(state);
    return;
  }
  lv_timer_reset(state->typing_timer);
  lv_timer_resume(state->typing_timer);
}

void ResultsPressed(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  if (state != nullptr && state->keyboard != nullptr) {
    HideSharedKeyboard(state->keyboard);
  }
}

lv_obj_t* AddLanguageButton(TranslatorViewState* state, int x, int width, lv_event_cb_t callback,
    lv_obj_t** label) {
  lv_obj_t* button = lv_button_create(state->root);
  lv_obj_set_size(button, width, kBarHeight);
  lv_obj_set_pos(button, x, kBarTop);
  lv_obj_set_style_radius(button, 22, LV_PART_MAIN);
  lv_obj_set_style_bg_color(button, lv_color_hex(kCard), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, state);
  *label = CreateLabel(button, "…", Colors().on_surface, &lvgl_font_google_sans_flex_24);
  lv_obj_set_width(*label, width - 24);
  lv_label_set_long_mode(*label, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(*label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_center(*label);
  return button;
}

bool FindBundle(TranslatorViewState* state) {
  if (state->storage == nullptr ||
      (!state->storage->IsSdCardMounted() && !state->storage->EnsureSdCardMounted())) {
    return false;
  }
  const char* base = state->storage->SdCardBasePath();
  if (base == nullptr || base[0] == '\0') {
    return false;
  }
  std::snprintf(state->base_path, sizeof(state->base_path), "%s", base);
  char directory[96] = {};
  std::snprintf(directory, sizeof(directory), "%s/%s", base, kTranslatorDirectory);
  DIR* folder = opendir(directory);
  if (folder == nullptr) {
    return false;
  }
  // Several bundles: the biggest wins (Global holds every other one).
  uint64_t best = 0;
  for (dirent* entry = readdir(folder); entry != nullptr; entry = readdir(folder)) {
    const char* name = entry->d_name;
    const size_t length = std::strlen(name);
    if (name[0] == '.') {
      continue;
    }
    size_t stem = 0;
    if (length > 5 && strcasecmp(name + length - 5, ".svtr") == 0) {
      stem = length;
    } else if (length > 7 && strncasecmp(name + length - 7, ".svtraa", 7) == 0) {
      stem = length - 2;  // split parts: open by the base name
    } else {
      continue;
    }
    char path[160] = {};
    std::snprintf(path, sizeof(path), "%s/%.*s", directory, static_cast<int>(stem), name);
    char probe[170] = {};
    std::snprintf(probe, sizeof(probe), "%s/%s", directory, name);
    struct stat info = {};
    const uint64_t size = stat(probe, &info) == 0 ? static_cast<uint64_t>(info.st_size) : 0;
    if (size >= best) {
      best = size;
      std::snprintf(state->pack_path, sizeof(state->pack_path), "%s", path);
    }
  }
  closedir(folder);
  return state->pack_path[0] != '\0';
}

void LoadBundle(void* context) {
  auto* state = static_cast<TranslatorViewState*>(context);
  if (!FindBundle(state) || !state->pack.Open(state->pack_path)) {
    ShowHint(state, "No dictionary on the SD card yet.\n\nPick a language bundle under "
        "Translator on the SurvivalOS web page, download it and copy the .svtr file anywhere "
        "on the card. It moves to /translator by itself.");
    return;
  }
  char code[16] = {};
  state->source = LoadText(kSourceKey, code, sizeof(code)) ? state->pack.FindLanguage(code) : -1;
  state->target = LoadText(kTargetKey, code, sizeof(code)) ? state->pack.FindLanguage(code) : -1;
  if (state->source < 0) state->source = state->pack.FindLanguage("cs");
  if (state->target < 0) state->target = state->pack.FindLanguage("en");
  if (state->source < 0) state->source = 0;
  if (state->target < 0) state->target = state->pack.LanguageCount() > 1 ? 1 : 0;
  state->loaded = true;
  RefreshLanguageButtons(state);
  RunTranslation(state);
  // Fonts and voices: unpacked once, in the background.
  StartJob(state, Job::kExtract);
  SetStatus(state, "Preparing fonts and voices...");
}

bool HandleBack(TranslatorViewState* state) {
  if (state->overlay != nullptr) {
    CloseOverlay(state);
    return true;
  }
  return false;
}

void ViewDeleted(lv_event_t* event) {
  auto* state = static_cast<TranslatorViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  lv_async_call_cancel(LoadBundle, state);
  if (state->typing_timer != nullptr) {
    lv_timer_delete(state->typing_timer);
  }
  if (state->poll_timer != nullptr) {
    lv_timer_delete(state->poll_timer);
  }
  bool worker_stopped = state->worker == nullptr;
  if (state->worker != nullptr) {
    // Let a running job finish first; synthesis of a sentence takes seconds.
    for (int i = 0; i < 300 && state->job_state.load() == static_cast<int>(JobState::kRunning);
         ++i) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (state->job_state.load() != static_cast<int>(JobState::kRunning)) {
      state->job.store(static_cast<int>(Job::kStop));
      xSemaphoreGive(state->wake);
      worker_stopped = xSemaphoreTake(state->worker_done, pdMS_TO_TICKS(5000)) == pdTRUE;
    }
  }
  // The labels go before the fonts they are drawn with.
  lv_obj_clean(state->root);
  for (lv_font_t* font : {state->big_font, state->small_font, state->big_cjk, state->small_cjk}) {
    if (font != nullptr) {
      lv_tiny_ttf_destroy(font);
    }
  }
  if (!worker_stopped) {
    LogMessage(LogLevel::kError, __FILE__, __LINE__, "Translator worker still busy\n");
    return;  // the worker keeps the state
  }
  if (state->wake != nullptr) vSemaphoreDelete(state->wake);
  if (state->worker_done != nullptr) vSemaphoreDelete(state->worker_done);
  heap_caps_free(state->records);
  heap_caps_free(state->scratch);
  state->pack.Close();
  state->~TranslatorViewState();
  heap_caps_free(state);
}

}  // namespace

lv_obj_t* CreateTranslatorView(lv_obj_t* parent, const app::AppEntry& app_entry,
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
  lv_obj_set_style_bg_color(container, lv_color_hex(Colors().surface), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container, LV_OPA_COVER, LV_PART_MAIN);

  // The state is large (two record buffers, the reader), so it lives in PSRAM.
  void* memory = heap_caps_calloc(1, sizeof(TranslatorViewState), MALLOC_CAP_SPIRAM);
  if (memory == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  auto* state = new (memory) TranslatorViewState();
  state->records = static_cast<TranslatorRecord*>(
      heap_caps_calloc(kRecordCapacity, sizeof(TranslatorRecord), MALLOC_CAP_SPIRAM));
  state->scratch = static_cast<TranslatorRecord*>(
      heap_caps_calloc(1, sizeof(TranslatorRecord), MALLOC_CAP_SPIRAM));
  state->root = container;
  state->storage = config.storage;
  state->audio = config.audio;
  state->width = config.width;
  state->height = config.height;
  lv_obj_add_event_cb(container, ViewDeleted, LV_EVENT_DELETE, state);
  if (state->records == nullptr || state->scratch == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }

  lv_obj_t* title = CreateLabel(container, "Translator", Colors().on_surface,
      &lvgl_font_google_sans_flex_36);
  lv_obj_set_pos(title, kSidePadding, kTitleTop);
  lv_obj_t* phrases = lv_button_create(container);
  lv_obj_set_size(phrases, LV_SIZE_CONTENT, 56);
  lv_obj_align(phrases, LV_ALIGN_TOP_RIGHT, -kSidePadding, kTitleTop - 6);
  lv_obj_set_style_radius(phrases, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(phrases, lv_color_hex(kChip), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(phrases, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(phrases, 22, LV_PART_MAIN);
  lv_obj_add_event_cb(phrases, PhrasesClicked, LV_EVENT_CLICKED, state);
  lv_obj_t* phrases_label = CreateLabel(phrases, "Phrases", 0xFFFFFF,
      &lvgl_font_google_sans_flex_24);
  lv_obj_center(phrases_label);

  const int swap_size = kBarHeight;
  const int button_width = (config.width - 2 * kSidePadding - swap_size - 20) / 2;
  state->source_button = AddLanguageButton(state, kSidePadding, button_width, SourceClicked,
      &state->source_label);
  lv_obj_t* swap = lv_button_create(container);
  lv_obj_set_size(swap, swap_size, swap_size);
  lv_obj_set_pos(swap, kSidePadding + button_width + 10, kBarTop);
  lv_obj_set_style_radius(swap, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(swap, lv_color_hex(kChip), LV_PART_MAIN);
  lv_obj_set_style_shadow_width(swap, 0, LV_PART_MAIN);
  lv_obj_add_event_cb(swap, SwapClicked, LV_EVENT_CLICKED, state);
  lv_obj_t* swap_icon = CreateLabel(swap, icon::kSwapHoriz, 0xFFFFFF,
      &lvgl_font_material_symbols_fill_32);
  lv_obj_center(swap_icon);
  state->target_button = AddLanguageButton(state,
      kSidePadding + button_width + 20 + swap_size, button_width, TargetClicked,
      &state->target_label);

  state->input = lv_textarea_create(container);
  lv_textarea_set_one_line(state->input, true);
  lv_textarea_set_placeholder_text(state->input, "Type a word or a sentence");
  lv_obj_set_size(state->input, config.width - 2 * kSidePadding, kInputHeight);
  lv_obj_set_pos(state->input, kSidePadding, kInputTop);
  lv_obj_set_style_radius(state->input, 22, LV_PART_MAIN);
  lv_obj_set_style_text_font(state->input, &lvgl_font_google_sans_flex_28, LV_PART_MAIN);
  lv_obj_add_event_cb(state->input, InputChanged, LV_EVENT_VALUE_CHANGED, state);
  lv_obj_add_event_cb(state->input, InputChanged, LV_EVENT_READY, state);

  state->results = lv_obj_create(container);
  MakePlain(state->results);
  lv_obj_add_flag(state->results, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(state->results, LV_DIR_VER);
  lv_obj_set_size(state->results, config.width, config.height - kResultsTop);
  lv_obj_set_pos(state->results, 0, kResultsTop);
  lv_obj_set_flex_flow(state->results, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(state->results, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
      LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(state->results, 12, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(state->results, 60, LV_PART_MAIN);
  lv_obj_add_event_cb(state->results, ResultsPressed, LV_EVENT_PRESSED, state);
  ShowHint(state, "Opening the dictionary...");

  state->status = CreateLabel(container, "", 0xFFFFFF, &lvgl_font_google_sans_flex_22);
  lv_obj_set_style_bg_color(state->status, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(state->status, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(state->status, 16, LV_PART_MAIN);
  lv_obj_set_style_pad_hor(state->status, 18, LV_PART_MAIN);
  lv_obj_set_style_pad_ver(state->status, 10, LV_PART_MAIN);
  lv_obj_align(state->status, LV_ALIGN_BOTTOM_MID, 0, -48);
  lv_obj_add_flag(state->status, LV_OBJ_FLAG_HIDDEN);

  SharedKeyboardConfig keyboard_config;
  keyboard_config.width = config.width;
  keyboard_config.height = config.height / 3;
  state->keyboard = CreateSharedKeyboard(container, keyboard_config);
  if (state->keyboard != nullptr) {
    AttachSharedKeyboardToTextArea(state->keyboard, state->input, nullptr);
  }

  state->typing_timer = lv_timer_create(TypingTimer, kTypingPauseMs, state);
  lv_timer_pause(state->typing_timer);
  state->poll_timer = lv_timer_create(PollTimer, kPollMs, state);

  state->wake = xSemaphoreCreateBinary();
  state->worker_done = xSemaphoreCreateBinary();
  if (state->wake != nullptr && state->worker_done != nullptr &&
      xTaskCreate(WorkerTask, "translator", kWorkerStack, state, 3, &state->worker) != pdPASS) {
    state->worker = nullptr;
  }

  RegisterConditionalBackNavigationHandler(container, [state]() { return HandleBack(state); });
  // Opened after the first frame so the app appears at once.
  lv_async_call(LoadBundle, state);
  return container;
}

}  // namespace lilygo_box::ui
