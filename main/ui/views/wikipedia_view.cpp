/*
 * @Description: Offline Wikipedia app view
 * @License: GPL 3.0
 */
#include "ui/views/wikipedia_view.h"

#include <dirent.h>
#include <strings.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "base/logger.h"
#include "hal/providers/storage_provider.h"
#include "survival/wiki_html.h"
#include "survival/zim_reader.h"
#include "ui/haptic_feedback.h"
#include "ui/input/back_navigation_controller.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/resources/fonts/icon_assets.h"
#include "ui/theme/theme_provider.h"
#include "ui/widgets/shared_keyboard.h"

namespace lilygo_box::ui {
namespace {

constexpr const char* kWikiDirectory = "wiki";
constexpr size_t kMaxArchives = 8;
constexpr size_t kMaxPathLength = 192;
constexpr size_t kMaxQueryLength = 64;
// One screenful plus a bit; the list is rebuilt as the query changes anyway.
constexpr uint32_t kMaxResults = 40;
constexpr size_t kHistoryDepth = 24;

constexpr int kSidePadding = 18;
constexpr int kHeaderHeight = 100;
constexpr int kSearchFieldHeight = 76;
constexpr int kResultRowHeight = 84;
constexpr int kArchiveRowHeight = 96;
constexpr int kBlockSpacing = 14;

// One archive found on the card. Kept as a struct so the list stays sortable.
struct ArchiveInfo {
  char path[kMaxPathLength] = {};
  char name[64] = {};
};

enum class Screen : uint8_t {
  kLibrary,
  kLoading,
  kBrowse,
  kArticle,
};

struct WikipediaViewState {
  hal::StorageProvider* storage = nullptr;
  lv_obj_t* root = nullptr;
  lv_obj_t* header_title = nullptr;
  lv_obj_t* body = nullptr;
  lv_obj_t* search_field = nullptr;
  lv_obj_t* keyboard = nullptr;
  lv_obj_t* status_label = nullptr;

  Screen screen = Screen::kLibrary;
  int view_width = 0;
  int view_height = 0;

  ArchiveInfo archives[kMaxArchives] = {};
  size_t archive_count = 0;
  int pending_archive = -1;

  survival::ZimArchive archive;
  char archive_label[64] = {};

  char query[kMaxQueryLength] = {};
  uint32_t result_indices[kMaxResults] = {};
  uint32_t result_count = 0;

  survival::WikiDocument document;
  uint32_t history[kHistoryDepth] = {};
  size_t history_depth = 0;
  char article_title[survival::kZimTitleMaxLength] = {};
};

/**
 * @brief Strips background, border and padding from an object.
 * @param object Object to flatten.
 */
void MakeTransparent(lv_obj_t* object) {
  lv_obj_set_style_bg_opa(object, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(object, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(object, 0, LV_PART_MAIN);
}

/**
 * @brief Creates a styled label.
 * @param parent Parent object.
 * @param text Initial text.
 * @param color Text colour in 0xRRGGBB form.
 * @param font Font to use.
 * @return The label, or nullptr on failure.
 */
lv_obj_t* CreateLabel(lv_obj_t* parent, const char* text, uint32_t color,
    const lv_font_t* font) {
  lv_obj_t* label = lv_label_create(parent);
  if (label == nullptr) {
    return nullptr;
  }
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
  return label;
}

/**
 * @brief Builds the directory the archives live in.
 * @param state View state.
 * @param buffer Output buffer.
 * @param size Buffer size.
 * @return true when the SD card is mounted and the path was built.
 */
bool BuildDirectoryPath(
    const WikipediaViewState* state, char* buffer, size_t size) {
  if (state->storage == nullptr || !state->storage->IsSdCardMounted()) {
    return false;
  }
  const char* base = state->storage->SdCardBasePath();
  if (base == nullptr || base[0] == '\0') {
    return false;
  }
  return std::snprintf(buffer, size, "%s/%s", base, kWikiDirectory) <
      static_cast<int>(size);
}

/**
 * @brief Turns an archive filename into something readable.
 * @param filename Filename on the card.
 * @param label Output label.
 * @param size Output buffer size.
 */
void MakeArchiveLabel(const char* filename, char* label, size_t size) {
  std::snprintf(label, size, "%s", filename);
  // Trim the extension, including a split suffix like `.zimaa`.
  char* dot = std::strrchr(label, '.');
  if (dot != nullptr) {
    *dot = '\0';
  }
  for (char* cursor = label; *cursor != '\0'; ++cursor) {
    if (*cursor == '_') {
      *cursor = ' ';
    }
  }
}

/**
 * @brief Finds the ZIM archives on the card.
 *
 * Split archives contribute only their first part, since opening that one pulls
 * in the rest.
 *
 * @param state View state.
 */
void ScanArchives(WikipediaViewState* state) {
  state->archive_count = 0;
  char directory[kMaxPathLength] = {};
  if (!BuildDirectoryPath(state, directory, sizeof(directory))) {
    return;
  }

  DIR* handle = opendir(directory);
  if (handle == nullptr) {
    return;
  }
  while (state->archive_count < kMaxArchives) {
    const dirent* entry = readdir(handle);
    if (entry == nullptr) {
      break;
    }
    const size_t length = std::strlen(entry->d_name);
    const bool whole = length > 4 &&
        strncasecmp(entry->d_name + length - 4, ".zim", 4) == 0;
    const bool first_part = length > 6 &&
        strncasecmp(entry->d_name + length - 6, ".zimaa", 6) == 0;
    if (!whole && !first_part) {
      continue;
    }

    char base[kMaxPathLength] = {};
    if (first_part) {
      // SplitFile wants the name without the part suffix.
      const size_t base_length = std::min(length - 2, sizeof(base) - 1);
      std::memcpy(base, entry->d_name, base_length);
      base[base_length] = '\0';
    } else {
      std::snprintf(base, sizeof(base), "%s", entry->d_name);
    }

    ArchiveInfo& archive = state->archives[state->archive_count];
    std::snprintf(
        archive.path, sizeof(archive.path), "%s/%s", directory, base);
    MakeArchiveLabel(entry->d_name, archive.name, sizeof(archive.name));
    ++state->archive_count;
  }
  closedir(handle);

  std::sort(state->archives, state->archives + state->archive_count,
      [](const ArchiveInfo& left, const ArchiveInfo& right) {
        return std::strcmp(left.name, right.name) < 0;
      });
}

void ShowLibrary(WikipediaViewState* state);
void ShowBrowse(WikipediaViewState* state);
void OpenArticleByIndex(WikipediaViewState* state, uint32_t entry_index,
    bool push_history);

/**
 * @brief Clears the body container and the on-screen keyboard.
 * @param state View state.
 */
void ResetBody(WikipediaViewState* state) {
  if (state->keyboard != nullptr) {
    HideSharedKeyboard(state->keyboard);
    state->keyboard = nullptr;
  }
  state->search_field = nullptr;
  state->status_label = nullptr;
  if (state->body != nullptr) {
    lv_obj_clean(state->body);
  }
}

/**
 * @brief Sets the text in the header.
 * @param state View state.
 * @param text Header text.
 */
void SetHeader(WikipediaViewState* state, const char* text) {
  if (state->header_title != nullptr) {
    lv_label_set_text(state->header_title, text);
  }
}

/**
 * @brief Opens the archive the user picked.
 * @param context View state.
 */
void OpenPendingArchive(void* context) {
  auto* state = static_cast<WikipediaViewState*>(context);
  if (state == nullptr || state->pending_archive < 0) {
    return;
  }
  const int index = state->pending_archive;
  state->pending_archive = -1;

  if (!state->archive.Open(state->archives[index].path)) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    ShowLibrary(state);
    if (state->status_label != nullptr) {
      lv_label_set_text(
          state->status_label, "That archive could not be opened.");
    }
    return;
  }
  std::snprintf(state->archive_label, sizeof(state->archive_label), "%s",
      state->archives[index].name);
  state->query[0] = '\0';
  PlayUiHapticEffect(HapticEffect::kSuccess);
  ShowBrowse(state);
}

/**
 * @brief Opens the tapped archive.
 * @param event LVGL event.
 */
void ArchiveClickedCallback(lv_event_t* event) {
  auto* state = static_cast<WikipediaViewState*>(lv_event_get_user_data(event));
  lv_obj_t* row = lv_event_get_target_obj(event);
  if (state == nullptr || row == nullptr) {
    return;
  }
  const int index =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
  if (index < 0 || index >= static_cast<int>(state->archive_count)) {
    return;
  }

  PlayUiHapticEffect(HapticEffect::kImpactMedium);
  state->pending_archive = index;
  ResetBody(state);
  state->screen = Screen::kLoading;
  SetHeader(state, "Opening…");
  state->status_label = CreateLabel(state->body,
      "Reading the archive index.\nThe first open takes a few seconds.",
      theme::ActiveThemeColors().on_surface_variant,
      &lvgl_font_google_sans_flex_24);
  if (state->status_label != nullptr) {
    lv_obj_set_width(state->status_label, state->view_width - 2 * kSidePadding);
    lv_obj_set_style_text_align(
        state->status_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_align(state->status_label, LV_ALIGN_TOP_MID, 0, 80);
  }
  // Let the loading text paint before the blocking read starts.
  lv_async_call(OpenPendingArchive, state);
}

/**
 * @brief Shows the list of archives found on the card.
 * @param state View state.
 */
void ShowLibrary(WikipediaViewState* state) {
  ResetBody(state);
  state->screen = Screen::kLibrary;
  state->archive.Close();
  SetHeader(state, "Wikipedia");

  ScanArchives(state);
  if (state->archive_count == 0) {
    state->status_label = CreateLabel(state->body,
        "No archives found.\n\nPut Kiwix ZIM files in /wiki on the SD card. "
        "Files larger than 4 GB have to be split into .zimaa, .zimab, … parts "
        "because the card is FAT32.",
        theme::ActiveThemeColors().on_surface_variant,
        &lvgl_font_google_sans_flex_24);
    if (state->status_label != nullptr) {
      lv_obj_set_width(
          state->status_label, state->view_width - 2 * kSidePadding);
      lv_label_set_long_mode(state->status_label, LV_LABEL_LONG_WRAP);
      lv_obj_align(state->status_label, LV_ALIGN_TOP_LEFT, 0, 20);
    }
    return;
  }

  for (size_t index = 0; index < state->archive_count; ++index) {
    lv_obj_t* row = lv_obj_create(state->body);
    if (row == nullptr) {
      return;
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, state->view_width - 2 * kSidePadding,
        kArchiveRowHeight);
    lv_obj_set_pos(row, 0, static_cast<int>(index) * (kArchiveRowHeight + 8));
    lv_obj_set_style_radius(row, 20, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row,
        lv_color_hex(theme::ActiveThemeColors().surface_container_high),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_user_data(
        row, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    lv_obj_add_event_cb(row, ArchiveClickedCallback, LV_EVENT_CLICKED, state);

    lv_obj_t* glyph = CreateLabel(row, icon::kMenuBook,
        theme::ActiveThemeColors().on_surface_variant,
        &lvgl_font_material_symbols_fill_32);
    if (glyph != nullptr) {
      lv_obj_align(glyph, LV_ALIGN_LEFT_MID, 22, 0);
    }
    lv_obj_t* name = CreateLabel(row, state->archives[index].name,
        theme::ActiveThemeColors().on_surface,
        &lvgl_font_google_sans_flex_24);
    if (name != nullptr) {
      lv_obj_set_width(name, state->view_width - 2 * kSidePadding - 100);
      lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
      lv_obj_align(name, LV_ALIGN_LEFT_MID, 76, 0);
    }
  }
}

/**
 * @brief Opens the tapped search result.
 * @param event LVGL event.
 */
void ResultClickedCallback(lv_event_t* event) {
  auto* state = static_cast<WikipediaViewState*>(lv_event_get_user_data(event));
  lv_obj_t* row = lv_event_get_target_obj(event);
  if (state == nullptr || row == nullptr) {
    return;
  }
  const int slot =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(row)));
  if (slot < 0 || slot >= static_cast<int>(state->result_count)) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  state->history_depth = 0;
  OpenArticleByIndex(state, state->result_indices[slot], true);
}

/**
 * @brief Rebuilds the result rows under the search field.
 * @param state View state.
 */
void RenderResults(WikipediaViewState* state) {
  lv_obj_t* list = lv_obj_get_child(state->body, -1);
  (void)list;

  // The results live in their own container so the search field is not rebuilt
  // on every keystroke, which would drop focus and hide the keyboard.
  lv_obj_t* results = nullptr;
  for (uint32_t index = 0; index < lv_obj_get_child_count(state->body);
      ++index) {
    lv_obj_t* child = lv_obj_get_child(state->body, index);
    if (lv_obj_get_user_data(child) ==
        reinterpret_cast<void*>(static_cast<intptr_t>(-2))) {
      results = child;
      break;
    }
  }
  if (results == nullptr) {
    return;
  }
  lv_obj_clean(results);

  if (state->result_count == 0) {
    lv_obj_t* empty = CreateLabel(results,
        state->query[0] == '\0' ? "Type to search." : "Nothing found.",
        theme::ActiveThemeColors().on_surface_variant,
        &lvgl_font_google_sans_flex_24);
    if (empty != nullptr) {
      lv_obj_align(empty, LV_ALIGN_TOP_LEFT, 4, 16);
    }
    return;
  }

  for (uint32_t slot = 0; slot < state->result_count; ++slot) {
    survival::ZimEntry entry;
    if (!state->archive.ReadEntry(state->result_indices[slot], &entry)) {
      continue;
    }
    lv_obj_t* row = lv_obj_create(results);
    if (row == nullptr) {
      return;
    }
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(row, state->view_width - 2 * kSidePadding,
        kResultRowHeight);
    lv_obj_set_pos(row, 0, static_cast<int>(slot) * (kResultRowHeight + 6));
    lv_obj_set_style_radius(row, 16, LV_PART_MAIN);
    lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row,
        lv_color_hex(theme::ActiveThemeColors().surface_container),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_user_data(
        row, reinterpret_cast<void*>(static_cast<intptr_t>(slot)));
    lv_obj_add_event_cb(row, ResultClickedCallback, LV_EVENT_CLICKED, state);

    lv_obj_t* title = CreateLabel(row, entry.title,
        theme::ActiveThemeColors().on_surface,
        &lvgl_font_google_sans_flex_24);
    if (title != nullptr) {
      lv_obj_set_width(title, state->view_width - 2 * kSidePadding - 40);
      lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
      lv_obj_align(title, LV_ALIGN_LEFT_MID, 20, 0);
    }
  }
}

/**
 * @brief Runs a title search for the current query.
 * @param state View state.
 */
void RunSearch(WikipediaViewState* state) {
  state->result_count = 0;
  if (state->archive.IsOpen()) {
    state->result_count = state->archive.SearchTitles(
        state->query, state->result_indices, kMaxResults);
  }
  RenderResults(state);
}

/**
 * @brief Re-runs the search whenever the query text changes.
 * @param event LVGL event.
 */
void SearchChangedCallback(lv_event_t* event) {
  auto* state = static_cast<WikipediaViewState*>(lv_event_get_user_data(event));
  lv_obj_t* field = lv_event_get_target_obj(event);
  if (state == nullptr || field == nullptr) {
    return;
  }
  const char* text = lv_textarea_get_text(field);
  std::snprintf(state->query, sizeof(state->query), "%s",
      text == nullptr ? "" : text);
  RunSearch(state);
}

/**
 * @brief Shows the search field and result list for the open archive.
 * @param state View state.
 */
void ShowBrowse(WikipediaViewState* state) {
  ResetBody(state);
  state->screen = Screen::kBrowse;
  survival::FreeWikiDocument(&state->document);
  SetHeader(state, state->archive_label);

  state->search_field = lv_textarea_create(state->body);
  if (state->search_field == nullptr) {
    return;
  }
  lv_textarea_set_one_line(state->search_field, true);
  lv_textarea_set_placeholder_text(state->search_field, "Search titles");
  lv_textarea_set_text(state->search_field, state->query);
  lv_obj_set_size(state->search_field, state->view_width - 2 * kSidePadding,
      kSearchFieldHeight);
  lv_obj_set_pos(state->search_field, 0, 0);
  lv_obj_set_style_radius(state->search_field, 20, LV_PART_MAIN);
  lv_obj_set_style_text_font(
      state->search_field, &lvgl_font_google_sans_flex_24, LV_PART_MAIN);
  lv_obj_add_event_cb(state->search_field, SearchChangedCallback,
      LV_EVENT_VALUE_CHANGED, state);

  lv_obj_t* results = lv_obj_create(state->body);
  if (results == nullptr) {
    return;
  }
  MakeTransparent(results);
  lv_obj_set_size(results, state->view_width - 2 * kSidePadding,
      state->view_height - kHeaderHeight - kSearchFieldHeight - 40);
  lv_obj_set_pos(results, 0, kSearchFieldHeight + 16);
  lv_obj_set_scroll_dir(results, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(results, LV_SCROLLBAR_MODE_OFF);
  // Marker so RenderResults() can find this container again.
  lv_obj_set_user_data(
      results, reinterpret_cast<void*>(static_cast<intptr_t>(-2)));

  SharedKeyboardConfig keyboard_config;
  keyboard_config.width = state->view_width;
  keyboard_config.height = state->view_height / 3;
  state->keyboard = CreateSharedKeyboard(state->root, keyboard_config);
  if (state->keyboard != nullptr) {
    AttachSharedKeyboardToTextArea(
        state->keyboard, state->search_field, nullptr);
  }

  RunSearch(state);
}

/**
 * @brief Follows a tapped link inside an article paragraph.
 * @param event LVGL event.
 */
void ArticleBlockClickedCallback(lv_event_t* event) {
  auto* state = static_cast<WikipediaViewState*>(lv_event_get_user_data(event));
  lv_obj_t* label = lv_event_get_target_obj(event);
  if (state == nullptr || label == nullptr) {
    return;
  }
  const int block_index =
      static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(label)));
  if (block_index < 0 ||
      block_index >= static_cast<int>(state->document.block_count)) {
    return;
  }
  const survival::WikiBlock& block = state->document.blocks[block_index];
  if (block.link_count == 0) {
    return;
  }

  lv_indev_t* input = lv_indev_active();
  if (input == nullptr) {
    return;
  }
  lv_point_t point = {};
  lv_indev_get_point(input, &point);
  lv_area_t area;
  lv_obj_get_coords(label, &area);
  point.x -= area.x1;
  point.y -= area.y1;

  const uint32_t character = lv_label_get_letter_on(label, &point, false);
  for (uint32_t index = 0; index < block.link_count; ++index) {
    const survival::WikiLink& link = block.links[index];
    if (character < link.start_char || character >= link.end_char) {
      continue;
    }
    char url[survival::kZimUrlMaxLength] = {};
    if (!survival::ResolveWikiHref(link.href, url, sizeof(url))) {
      PlayUiHapticEffect(HapticEffect::kWarning);
      return;
    }
    uint32_t entry_index = 0;
    if (!state->archive.FindByUrl('C', url, &entry_index) &&
        !state->archive.FindByUrl('A', url, &entry_index)) {
      PlayUiHapticEffect(HapticEffect::kWarning);
      return;
    }
    PlayUiHapticEffect(HapticEffect::kImpactLight);
    OpenArticleByIndex(state, entry_index, true);
    return;
  }
}

/**
 * @brief Picks the font a block is drawn in.
 * @param type Block type.
 * @return Font for that block.
 */
const lv_font_t* FontForBlock(survival::WikiBlockType type) {
  switch (type) {
    case survival::WikiBlockType::kTitle:
      return &lvgl_font_google_sans_flex_36;
    case survival::WikiBlockType::kHeading:
      return &lvgl_font_google_sans_flex_32;
    case survival::WikiBlockType::kSubheading:
      return &lvgl_font_google_sans_flex_28;
    default:
      return &lvgl_font_google_sans_flex_24;
  }
}

/**
 * @brief Lays the parsed article out in the body container.
 * @param state View state.
 */
void RenderArticle(WikipediaViewState* state) {
  ResetBody(state);
  state->screen = Screen::kArticle;
  SetHeader(state, state->article_title);

  const int width = state->view_width - 2 * kSidePadding;
  int y = 0;
  for (uint32_t index = 0; index < state->document.block_count; ++index) {
    const survival::WikiBlock& block = state->document.blocks[index];
    if (block.text == nullptr) {
      continue;
    }
    lv_obj_t* label = lv_label_create(state->body);
    if (label == nullptr) {
      break;
    }
    lv_label_set_recolor(label, true);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, block.text);
    lv_obj_set_width(label, block.type == survival::WikiBlockType::kListItem
            ? width - 24
            : width);
    lv_obj_set_style_text_font(label, FontForBlock(block.type), LV_PART_MAIN);
    lv_obj_set_style_text_color(label,
        lv_color_hex(block.type == survival::WikiBlockType::kParagraph ||
                block.type == survival::WikiBlockType::kListItem
                ? theme::ActiveThemeColors().on_surface
                : theme::ActiveThemeColors().on_surface),
        LV_PART_MAIN);
    lv_obj_set_style_text_line_space(label, 6, LV_PART_MAIN);
    lv_obj_set_pos(label,
        block.type == survival::WikiBlockType::kListItem ? 24 : 0, y);
    lv_obj_set_user_data(
        label, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    if (block.link_count > 0) {
      lv_obj_add_flag(label, LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_event_cb(
          label, ArticleBlockClickedCallback, LV_EVENT_CLICKED, state);
    }

    lv_obj_update_layout(label);
    y += lv_obj_get_height(label) + kBlockSpacing;
    if (block.type == survival::WikiBlockType::kHeading ||
        block.type == survival::WikiBlockType::kSubheading) {
      y += 8;
    }
  }

  if (state->document.truncated) {
    lv_obj_t* note = CreateLabel(state->body,
        "— the rest of this article was too long to show —",
        theme::ActiveThemeColors().on_surface_variant,
        &lvgl_font_google_sans_flex_22);
    if (note != nullptr) {
      lv_obj_set_width(note, width);
      lv_label_set_long_mode(note, LV_LABEL_LONG_WRAP);
      lv_obj_set_pos(note, 0, y + 10);
    }
  }
  lv_obj_scroll_to_y(state->body, 0, LV_ANIM_OFF);
}

void OpenArticleByIndex(WikipediaViewState* state, uint32_t entry_index,
    bool push_history) {
  survival::ZimEntry entry;
  if (!state->archive.ReadEntry(entry_index, &entry) ||
      !state->archive.ResolveEntry(&entry)) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }

  survival::ZimBlob blob;
  if (!state->archive.ReadBlob(entry.cluster, entry.blob, &blob)) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }

  survival::WikiDocument document;
  const bool parsed = survival::ParseWikiHtml(
      reinterpret_cast<const char*>(blob.data), blob.size, &document);
  state->archive.FreeBlob(&blob);
  if (!parsed) {
    PlayUiHapticEffect(HapticEffect::kWarning);
    return;
  }

  if (push_history && state->history_depth < kHistoryDepth) {
    state->history[state->history_depth++] = entry.index;
  }
  survival::FreeWikiDocument(&state->document);
  state->document = document;
  std::snprintf(
      state->article_title, sizeof(state->article_title), "%s", entry.title);
  RenderArticle(state);
}

/**
 * @brief Handles the system back gesture inside the app.
 * @param state View state.
 * @return true when the app consumed the gesture.
 */
bool HandleBack(WikipediaViewState* state) {
  switch (state->screen) {
    case Screen::kArticle:
      // Walk back through the articles that were opened from links first.
      if (state->history_depth > 1) {
        --state->history_depth;
        OpenArticleByIndex(
            state, state->history[state->history_depth - 1], false);
        return true;
      }
      state->history_depth = 0;
      ShowBrowse(state);
      return true;
    case Screen::kBrowse:
      ShowLibrary(state);
      return true;
    case Screen::kLoading:
      return true;
    case Screen::kLibrary:
      // Nothing left to go back to; let the shell leave the app.
      return false;
  }
  return false;
}

/**
 * @brief Frees everything the view owns.
 * @param event LVGL event.
 */
void ViewDeletedCallback(lv_event_t* event) {
  auto* state = static_cast<WikipediaViewState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  survival::FreeWikiDocument(&state->document);
  state->archive.Close();
  delete state;
}

}  // namespace

lv_obj_t* CreateWikipediaView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config) {
  (void)app_entry;
  if (parent == nullptr || config.width <= 0 || config.height <= 0) {
    return nullptr;
  }

  lv_obj_t* container = lv_obj_create(parent);
  if (container == nullptr) {
    return nullptr;
  }
  lv_obj_remove_flag(container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(container, config.width, config.height);
  lv_obj_align(container, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(container, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(container,
      lv_color_hex(theme::ActiveThemeColors().surface), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(container, LV_OPA_COVER, LV_PART_MAIN);

  auto* state = new (std::nothrow) WikipediaViewState();
  if (state == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  state->root = container;
  state->storage = config.storage;
  state->view_width = config.width;
  state->view_height = config.height;
  lv_obj_add_event_cb(container, ViewDeletedCallback, LV_EVENT_DELETE, state);

  state->header_title = CreateLabel(container, "Wikipedia",
      theme::ActiveThemeColors().on_surface, &lvgl_font_google_sans_flex_36);
  if (state->header_title != nullptr) {
    lv_obj_set_width(state->header_title, config.width - 2 * kSidePadding);
    lv_label_set_long_mode(state->header_title, LV_LABEL_LONG_DOT);
    lv_obj_align(state->header_title, LV_ALIGN_TOP_LEFT, kSidePadding,
        kHeaderHeight - 52);
  }

  state->body = lv_obj_create(container);
  if (state->body == nullptr) {
    lv_obj_delete(container);
    return nullptr;
  }
  MakeTransparent(state->body);
  lv_obj_set_size(state->body, config.width - 2 * kSidePadding,
      config.height - kHeaderHeight);
  lv_obj_set_pos(state->body, kSidePadding, kHeaderHeight);
  lv_obj_set_scroll_dir(state->body, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(state->body, LV_SCROLLBAR_MODE_OFF);

  RegisterConditionalBackNavigationHandler(
      container, [state]() { return HandleBack(state); });

  if (state->storage != nullptr) {
    state->storage->EnsureSdCardMounted();
  }
  ShowLibrary(state);
  return container;
}

}  // namespace lilygo_box::ui
