/*
 * @Description: Petdoku, a logic puzzle on a grid of regions
 * @License: GPL 3.0
 */
#include "ui/views/petdoku_view.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

#include "app/storage/littlefs_storage.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "games/petdoku_engine.h"
#include "ui/haptic_feedback.h"
#include "ui/resources/fonts/font_assets.h"
#include "ui/theme/theme_provider.h"

namespace lilygo_box::ui {
namespace {

using games::PetdokuCheck;
using games::PetdokuMark;
using games::PetdokuPuzzle;

constexpr int kSizeCount = games::kPetdokuMaxSize - games::kPetdokuMinSize + 1;
constexpr int kDefaultSize = 8;

// Layout. The board takes all the height between the header and the buttons,
// so its cells are taller than wide: the logic only needs N x N cells, and
// the upright screen gives far more room downwards than across.
constexpr int kSidePadding = 16;
constexpr int kTitleTop = 54;
constexpr int kSubtitleTop = 104;
constexpr int kBoardTop = 150;
constexpr int kBoardControlsGap = 22;
constexpr int kControlsHeight = 62;
constexpr int kControlsBottomMargin = 72;
constexpr int kControlsGap = 12;
// Cells are kept from getting more than 1.6 times as tall as they are wide.
constexpr int kMaxCellAspectTenths = 16;
// Gaps between cells, in pixels: within a region, between regions, around.
constexpr int kThinGap = 1;
constexpr int kRegionGap = 4;
constexpr int kOuterGap = 3;
constexpr int kBoardRadius = 10;

constexpr uint32_t kClockPeriodMs = 500;
constexpr uint32_t kSavePeriodMs = 20000;
constexpr uint32_t kGeneratorPollMs = 40;
constexpr uint32_t kConfirmWindowMs = 3000;
constexpr int kGenerateAttempts = 800;
constexpr uint32_t kGeneratorStackBytes = 12 * 1024;
constexpr UBaseType_t kGeneratorPriority = 2;
constexpr size_t kUndoCapacity = 600;

constexpr char kSaveFileName[] = "petdoku.bin";
constexpr uint32_t kSaveMagic = 0x314B4450;  // "PDK1"
constexpr uint16_t kSaveVersion = 1;

struct Palette {
  uint32_t gap;
  uint32_t regions[games::kPetdokuMaxSize];
  uint32_t pet;
  uint32_t conflict;
  uint32_t solved;
  uint32_t cross;
  uint32_t pressed;
};

// Muted, earthy tones that tell regions apart without shouting; the marks
// are the brightest thing on the board.
constexpr Palette kDarkPalette = {
    .gap = 0x050506,
    .regions = {0x2E3B4E, 0x3D4F3A, 0x5A3A33, 0x574A2A, 0x4A3651, 0x2C4A4A,
        0x553844, 0x45482E, 0x39434F, 0x5B5143},
    .pet = 0xF2F2F0,
    .conflict = 0xFF6B5E,
    .solved = 0xE9C46A,
    .cross = 0xA7ADB4,
    .pressed = 0xFFFFFF,
};

constexpr Palette kLightPalette = {
    .gap = 0x2A2B2E,
    .regions = {0xC9D3E6, 0xCFDCCB, 0xE6CFC6, 0xE8DDBF, 0xD9CCE0, 0xC6DEDC,
        0xE3CBD3, 0xD8DCBF, 0xCCD3DB, 0xE2DACB},
    .pet = 0x1C1C1E,
    .conflict = 0xC62828,
    .solved = 0x9A6B00,
    .cross = 0x5F6368,
    .pressed = 0x000000,
};

const Palette& ActivePalette() {
  const uint32_t surface = theme::ActiveThemeColors().surface;
  const uint32_t luminance = ((surface >> 16) & 0xFF) * 3 +
                             ((surface >> 8) & 0xFF) * 6 + (surface & 0xFF);
  return luminance < 128 * 10 ? kDarkPalette : kLightPalette;
}

struct UndoEntry {
  uint8_t cell;
  PetdokuMark previous;
  uint16_t gesture;
};

// What is kept on the internal flash between sessions.
struct SaveData {
  uint32_t magic;
  uint16_t version;
  uint8_t size;
  uint8_t solved;
  uint32_t number;
  uint32_t elapsed_ms;
  uint8_t region[games::kPetdokuMaxCells];
  uint8_t solution[games::kPetdokuMaxSize];
  uint8_t marks[games::kPetdokuMaxCells];
  uint32_t best_ms[kSizeCount];
  uint32_t next_number[kSizeCount];
  uint32_t solved_count[kSizeCount];
};

/**
 * @brief A puzzle being generated on a task of its own. Whichever of the task
 *        and the view lets go of it second deletes it.
 */
struct GeneratorJob {
  int size = 0;
  uint32_t number = 0;
  bool ok = false;
  PetdokuPuzzle puzzle;
  std::atomic<bool> done{false};
  std::atomic<bool> released{false};
};

void ReleaseJob(GeneratorJob* job) {
  if (job != nullptr && job->released.exchange(true)) {
    delete job;
  }
}

void GeneratorTask(void* context) {
  auto* job = static_cast<GeneratorJob*>(context);
  // Puzzle numbers are seeds, so "No. 12" is the same puzzle on every phone.
  job->ok = games::GeneratePetdoku(
      job->size, job->number, kGenerateAttempts, &job->puzzle);
  job->done.store(true);
  ReleaseJob(job);
  vTaskDelete(nullptr);
}

struct PetdokuState {
  int width = 0;
  int height = 0;
  lv_obj_t* root = nullptr;
  lv_obj_t* board = nullptr;
  lv_obj_t* clock_label = nullptr;
  lv_obj_t* subtitle = nullptr;
  lv_obj_t* undo_button = nullptr;
  lv_obj_t* clear_label = nullptr;
  lv_obj_t* size_label = nullptr;
  lv_obj_t* new_label = nullptr;
  lv_timer_t* clock_timer = nullptr;
  lv_timer_t* generator_timer = nullptr;
  GeneratorJob* job = nullptr;

  bool has_puzzle = false;
  PetdokuPuzzle puzzle;
  uint32_t number = 0;
  PetdokuMark marks[games::kPetdokuMaxCells] = {};
  PetdokuCheck check;
  bool solved = false;
  uint32_t elapsed_ms = 0;
  int64_t clock_started_us = 0;
  bool dirty = false;
  uint32_t since_save_ms = 0;

  uint32_t best_ms[kSizeCount] = {};
  uint32_t next_number[kSizeCount] = {};
  uint32_t solved_count[kSizeCount] = {};

  // Board geometry, recomputed for every puzzle.
  int cell_width = 0;
  int cell_height = 0;

  // The finger on the board.
  int pressed_cell = -1;
  int last_drag_cell = -1;
  bool drag_marks = false;
  uint16_t gesture = 0;
  UndoEntry undo[kUndoCapacity];
  size_t undo_count = 0;

  // Buttons that ask once more before throwing progress away.
  lv_obj_t* armed_label = nullptr;
  int64_t armed_until_us = 0;
  int armed_size = 0;
};

int SizeIndex(int size) { return size - games::kPetdokuMinSize; }

int64_t NowUs() { return esp_timer_get_time(); }

uint32_t ElapsedMs(const PetdokuState* state) {
  if (state->clock_started_us == 0) {
    return state->elapsed_ms;
  }
  return state->elapsed_ms +
         static_cast<uint32_t>((NowUs() - state->clock_started_us) / 1000);
}

void PauseClock(PetdokuState* state) {
  state->elapsed_ms = ElapsedMs(state);
  state->clock_started_us = 0;
}

void FormatDuration(uint32_t ms, char* buffer, size_t size) {
  const uint32_t seconds = ms / 1000;
  if (seconds >= 3600) {
    std::snprintf(buffer, size, "%lu:%02lu:%02lu",
        static_cast<unsigned long>(seconds / 3600),
        static_cast<unsigned long>(seconds / 60 % 60),
        static_cast<unsigned long>(seconds % 60));
  } else {
    std::snprintf(buffer, size, "%lu:%02lu",
        static_cast<unsigned long>(seconds / 60),
        static_cast<unsigned long>(seconds % 60));
  }
}

void SavePath(char* buffer, size_t size) {
  std::snprintf(buffer, size, "%s/%s", app::LittleFsStorageBasePath(),
      kSaveFileName);
}

void Save(PetdokuState* state) {
  if (!app::IsLittleFsStorageMounted()) {
    return;
  }
  auto data = std::make_unique<SaveData>();
  std::memset(data.get(), 0, sizeof(SaveData));
  data->magic = kSaveMagic;
  data->version = kSaveVersion;
  if (state->has_puzzle) {
    const int cells = state->puzzle.size * state->puzzle.size;
    data->size = static_cast<uint8_t>(state->puzzle.size);
    data->solved = state->solved ? 1 : 0;
    data->number = state->number;
    data->elapsed_ms = ElapsedMs(state);
    std::memcpy(data->region, state->puzzle.region, cells);
    std::memcpy(data->solution, state->puzzle.solution, state->puzzle.size);
    for (int cell = 0; cell < cells; ++cell) {
      data->marks[cell] = static_cast<uint8_t>(state->marks[cell]);
    }
  }
  std::memcpy(data->best_ms, state->best_ms, sizeof(data->best_ms));
  std::memcpy(data->next_number, state->next_number, sizeof(data->next_number));
  std::memcpy(
      data->solved_count, state->solved_count, sizeof(data->solved_count));
  char path[64];
  SavePath(path, sizeof(path));
  std::FILE* file = std::fopen(path, "wb");
  if (file != nullptr) {
    std::fwrite(data.get(), sizeof(SaveData), 1, file);
    std::fclose(file);
  }
  state->dirty = false;
  state->since_save_ms = 0;
}

/**
 * @return true when a saved puzzle was restored.
 */
bool Load(PetdokuState* state) {
  if (!app::IsLittleFsStorageMounted()) {
    return false;
  }
  char path[64];
  SavePath(path, sizeof(path));
  std::FILE* file = std::fopen(path, "rb");
  if (file == nullptr) {
    return false;
  }
  auto data = std::make_unique<SaveData>();
  const bool read = std::fread(data.get(), sizeof(SaveData), 1, file) == 1;
  std::fclose(file);
  if (!read || data->magic != kSaveMagic || data->version != kSaveVersion) {
    return false;
  }
  std::memcpy(state->best_ms, data->best_ms, sizeof(state->best_ms));
  std::memcpy(
      state->next_number, data->next_number, sizeof(state->next_number));
  std::memcpy(
      state->solved_count, data->solved_count, sizeof(state->solved_count));
  if (data->size < games::kPetdokuMinSize ||
      data->size > games::kPetdokuMaxSize) {
    return false;
  }
  const int cells = data->size * data->size;
  state->puzzle.size = data->size;
  std::memcpy(state->puzzle.region, data->region, cells);
  std::memcpy(state->puzzle.solution, data->solution, data->size);
  for (int cell = 0; cell < cells; ++cell) {
    if (data->region[cell] >= data->size || data->marks[cell] > 2) {
      return false;
    }
    state->marks[cell] = static_cast<PetdokuMark>(data->marks[cell]);
  }
  state->number = data->number;
  state->solved = data->solved != 0;
  state->elapsed_ms = data->elapsed_ms;
  state->has_puzzle = true;
  return true;
}

void UpdateSubtitle(PetdokuState* state) {
  if (state->subtitle == nullptr) {
    return;
  }
  char text[96];
  const theme::ThemeColors& colors = theme::ActiveThemeColors();
  uint32_t color = colors.on_surface_variant;
  if (!state->has_puzzle || state->job != nullptr) {
    std::snprintf(text, sizeof(text), "Preparing a puzzle...");
  } else if (state->solved) {
    char time_text[24];
    char best_text[24];
    FormatDuration(ElapsedMs(state), time_text, sizeof(time_text));
    FormatDuration(state->best_ms[SizeIndex(state->puzzle.size)], best_text,
        sizeof(best_text));
    std::snprintf(text, sizeof(text), "Solved in %s  \xC2\xB7  best %s",
        time_text, best_text);
    color = ActivePalette().solved;
  } else {
    std::snprintf(text, sizeof(text), "No. %lu  \xC2\xB7  %d \xC3\x97 %d",
        static_cast<unsigned long>(state->number), state->puzzle.size,
        state->puzzle.size);
  }
  lv_label_set_text(state->subtitle, text);
  lv_obj_set_style_text_color(state->subtitle, lv_color_hex(color), 0);
}

void UpdateClock(PetdokuState* state) {
  if (state->clock_label == nullptr) {
    return;
  }
  char text[32];
  FormatDuration(state->has_puzzle ? ElapsedMs(state) : 0, text, sizeof(text));
  lv_label_set_text(state->clock_label, text);
}

void UpdateButtons(PetdokuState* state) {
  if (state->size_label != nullptr) {
    char text[32];
    const int size = state->has_puzzle ? state->puzzle.size : kDefaultSize;
    std::snprintf(text, sizeof(text), "%d \xC3\x97 %d", size, size);
    lv_label_set_text(state->size_label, text);
  }
  if (state->new_label != nullptr) {
    lv_label_set_text(state->new_label, state->solved ? "Next" : "New");
  }
  if (state->clear_label != nullptr) {
    lv_label_set_text(state->clear_label, "Clear");
  }
  state->armed_label = nullptr;
}

/**
 * @brief Geometry of a cell, with the gaps that draw grid lines and region
 *        borders taken off its sides.
 */
lv_area_t CellArea(const PetdokuState* state, const lv_area_t& board, int row,
    int col, bool with_gaps) {
  const int size = state->puzzle.size;
  lv_area_t area;
  area.x1 = board.x1 + col * state->cell_width;
  area.y1 = board.y1 + row * state->cell_height;
  area.x2 = area.x1 + state->cell_width - 1;
  area.y2 = area.y1 + state->cell_height - 1;
  if (!with_gaps) {
    return area;
  }
  const uint8_t* region = state->puzzle.region;
  const int cell = row * size + col;
  const auto gap_to = [&](int other) {
    return region[other] == region[cell] ? kThinGap : kRegionGap;
  };
  area.x1 += col == 0 ? kOuterGap : gap_to(cell - 1) - gap_to(cell - 1) / 2;
  area.x2 -= col == size - 1 ? kOuterGap : gap_to(cell + 1) / 2;
  area.y1 +=
      row == 0 ? kOuterGap : gap_to(cell - size) - gap_to(cell - size) / 2;
  area.y2 -= row == size - 1 ? kOuterGap : gap_to(cell + size) / 2;
  return area;
}

void DrawCross(lv_layer_t* layer, const lv_area_t& cell, uint32_t color) {
  const int width = lv_area_get_width(&cell);
  const int height = lv_area_get_height(&cell);
  const int half = std::max(5, std::min(width, height) * 15 / 100);
  const int cx = cell.x1 + width / 2;
  const int cy = cell.y1 + height / 2;
  lv_draw_line_dsc_t line;
  lv_draw_line_dsc_init(&line);
  line.color = lv_color_hex(color);
  line.opa = 210;
  line.width = std::max(2, std::min(width, height) / 22);
  line.round_start = 1;
  line.round_end = 1;
  line.p1.x = cx - half;
  line.p1.y = cy - half;
  line.p2.x = cx + half;
  line.p2.y = cy + half;
  lv_draw_line(layer, &line);
  line.p1.x = cx + half;
  line.p2.x = cx - half;
  lv_draw_line(layer, &line);
}

// A pet is a seal: a ring with a dot in it.
void DrawPet(lv_layer_t* layer, const lv_area_t& cell, uint32_t color) {
  const int width = lv_area_get_width(&cell);
  const int height = lv_area_get_height(&cell);
  const int radius = std::max(8, std::min(width, height) * 30 / 100);
  const int cx = cell.x1 + width / 2;
  const int cy = cell.y1 + height / 2;
  lv_draw_rect_dsc_t ring;
  lv_draw_rect_dsc_init(&ring);
  ring.radius = LV_RADIUS_CIRCLE;
  ring.bg_opa = LV_OPA_TRANSP;
  ring.border_color = lv_color_hex(color);
  ring.border_opa = LV_OPA_COVER;
  ring.border_width = std::max(3, radius / 5);
  lv_area_t area = {cx - radius, cy - radius, cx + radius, cy + radius};
  lv_draw_rect(layer, &ring, &area);

  const int dot = std::max(3, radius * 42 / 100);
  lv_draw_rect_dsc_t fill;
  lv_draw_rect_dsc_init(&fill);
  fill.radius = LV_RADIUS_CIRCLE;
  fill.bg_color = lv_color_hex(color);
  fill.bg_opa = LV_OPA_COVER;
  area = {cx - dot, cy - dot, cx + dot, cy + dot};
  lv_draw_rect(layer, &fill, &area);
}

void BoardDrawCallback(lv_event_t* event) {
  auto* state = static_cast<PetdokuState*>(lv_event_get_user_data(event));
  lv_layer_t* layer = lv_event_get_layer(event);
  if (state == nullptr || layer == nullptr) {
    return;
  }
  const Palette& palette = ActivePalette();
  lv_area_t board;
  lv_obj_get_coords(state->board, &board);

  lv_draw_rect_dsc_t rect;
  lv_draw_rect_dsc_init(&rect);
  rect.bg_color = lv_color_hex(palette.gap);
  rect.bg_opa = LV_OPA_COVER;
  rect.radius = kBoardRadius;
  lv_draw_rect(layer, &rect, &board);
  if (!state->has_puzzle) {
    return;
  }

  const int size = state->puzzle.size;
  rect.radius = 0;
  for (int row = 0; row < size; ++row) {
    for (int col = 0; col < size; ++col) {
      const int cell = row * size + col;
      const lv_area_t area = CellArea(state, board, row, col, true);
      const uint32_t fill = palette.regions[state->puzzle.region[cell]];
      rect.bg_color = cell == state->pressed_cell
                          ? lv_color_mix(lv_color_hex(palette.pressed),
                                lv_color_hex(fill), 40)
                          : lv_color_hex(fill);
      lv_draw_rect(layer, &rect, &area);
      if (state->marks[cell] == PetdokuMark::kExcluded) {
        DrawCross(layer, area, palette.cross);
      } else if (state->marks[cell] == PetdokuMark::kPet) {
        DrawPet(layer, area,
            state->solved                ? palette.solved
            : state->check.conflict[cell] ? palette.conflict
                                         : palette.pet);
      }
    }
  }
}

void InvalidateCell(PetdokuState* state, int cell) {
  if (cell < 0 || state->board == nullptr || !state->has_puzzle) {
    return;
  }
  lv_area_t board;
  lv_obj_get_coords(state->board, &board);
  const int size = state->puzzle.size;
  lv_area_t area = CellArea(state, board, cell / size, cell % size, false);
  lv_obj_invalidate_area(state->board, &area);
}

void LayoutBoard(PetdokuState* state) {
  if (state->board == nullptr) {
    return;
  }
  const int size = state->has_puzzle ? state->puzzle.size : kDefaultSize;
  const int available_width = state->width - 2 * kSidePadding;
  const int controls_top =
      state->height - kControlsBottomMargin - kControlsHeight;
  const int available_height = controls_top - kBoardControlsGap - kBoardTop;
  state->cell_width = available_width / size;
  state->cell_height = std::min(available_height / size,
      state->cell_width * kMaxCellAspectTenths / 10);
  const int board_width = state->cell_width * size;
  const int board_height = state->cell_height * size;
  lv_obj_set_size(state->board, board_width, board_height);
  lv_obj_set_pos(state->board, (state->width - board_width) / 2,
      kBoardTop + (available_height - board_height) / 2);
  lv_obj_invalidate(state->board);
}

/**
 * @brief Re-checks the rules after a change and reacts to a solved board.
 * @return true when the set of conflicting pets changed, so every pet needs
 *         repainting.
 */
bool Evaluate(PetdokuState* state) {
  bool previous[games::kPetdokuMaxCells];
  std::memcpy(previous, state->check.conflict, sizeof(previous));
  games::CheckPetdoku(state->puzzle, state->marks, &state->check);
  const bool conflicts_changed =
      std::memcmp(previous, state->check.conflict, sizeof(previous)) != 0;
  if (state->check.solved && !state->solved) {
    state->solved = true;
    PauseClock(state);
    const int index = SizeIndex(state->puzzle.size);
    ++state->solved_count[index];
    if (state->best_ms[index] == 0 ||
        state->elapsed_ms < state->best_ms[index]) {
      state->best_ms[index] = state->elapsed_ms;
    }
    PlayUiHapticEffect(HapticEffect::kSuccess);
    UpdateSubtitle(state);
    UpdateButtons(state);
    Save(state);
    return true;
  }
  return conflicts_changed;
}

void SetMark(PetdokuState* state, int cell, PetdokuMark mark) {
  if (state->marks[cell] == mark) {
    return;
  }
  if (state->undo_count == kUndoCapacity) {
    // Forget the oldest change rather than refuse a new one.
    std::memmove(state->undo, state->undo + 1,
        sizeof(UndoEntry) * (kUndoCapacity - 1));
    --state->undo_count;
  }
  state->undo[state->undo_count++] = {static_cast<uint8_t>(cell),
      state->marks[cell], state->gesture};
  const bool pets_involved =
      mark == PetdokuMark::kPet || state->marks[cell] == PetdokuMark::kPet;
  state->marks[cell] = mark;
  state->dirty = true;
  if (Evaluate(state) || pets_involved) {
    lv_obj_invalidate(state->board);
  } else {
    InvalidateCell(state, cell);
  }
}

int CellAtPoint(const PetdokuState* state, const lv_point_t& point) {
  if (!state->has_puzzle || state->cell_width <= 0 || state->cell_height <= 0) {
    return -1;
  }
  lv_area_t board;
  lv_obj_get_coords(state->board, &board);
  const int x = point.x - board.x1;
  const int y = point.y - board.y1;
  const int size = state->puzzle.size;
  const int col = x / state->cell_width;
  const int row = y / state->cell_height;
  if (x < 0 || y < 0 || col >= size || row >= size) {
    return -1;
  }
  return row * size + col;
}

void BoardInputCallback(lv_event_t* event) {
  auto* state = static_cast<PetdokuState*>(lv_event_get_user_data(event));
  if (state == nullptr || !state->has_puzzle || state->solved ||
      state->job != nullptr) {
    return;
  }
  const lv_event_code_t code = lv_event_get_code(event);
  lv_indev_t* indev = lv_indev_active();
  lv_point_t point = {};
  if (indev != nullptr) {
    lv_indev_get_point(indev, &point);
  }

  if (code == LV_EVENT_PRESSED) {
    const int cell = CellAtPoint(state, point);
    if (cell < 0) {
      return;
    }
    ++state->gesture;
    state->pressed_cell = cell;
    state->last_drag_cell = cell;
    // A tap steps the cell: empty, ruled out, pet, empty. It happens on touch
    // down, not on release, so the board answers as fast as the finger lands.
    const PetdokuMark mark = state->marks[cell];
    const PetdokuMark next =
        mark == PetdokuMark::kEmpty      ? PetdokuMark::kExcluded
        : mark == PetdokuMark::kExcluded ? PetdokuMark::kPet
                                         : PetdokuMark::kEmpty;
    // Dragging on from an empty cell rules out every empty cell it crosses.
    state->drag_marks = mark == PetdokuMark::kEmpty;
    SetMark(state, cell, next);
    InvalidateCell(state, cell);
    PlayUiHapticEffect(HapticEffect::kPuzzleTap);
    lv_refr_now(nullptr);
    return;
  }

  if (code == LV_EVENT_PRESSING) {
    const int cell = CellAtPoint(state, point);
    if (cell < 0 || cell == state->last_drag_cell) {
      return;
    }
    const int previous = state->pressed_cell;
    state->last_drag_cell = cell;
    state->pressed_cell = cell;
    InvalidateCell(state, previous);
    if (state->drag_marks && state->marks[cell] == PetdokuMark::kEmpty) {
      SetMark(state, cell, PetdokuMark::kExcluded);
      PlayUiHapticEffect(HapticEffect::kPuzzleDrag);
    }
    InvalidateCell(state, cell);
    return;
  }

  if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
    const int previous = state->pressed_cell;
    state->pressed_cell = -1;
    state->last_drag_cell = -1;
    state->drag_marks = false;
    InvalidateCell(state, previous);
  }
}

void ApplyPuzzle(PetdokuState* state, const PetdokuPuzzle& puzzle,
    uint32_t number) {
  state->puzzle = puzzle;
  state->number = number;
  state->has_puzzle = true;
  std::memset(state->marks, 0, sizeof(state->marks));
  state->check = PetdokuCheck();
  state->solved = false;
  state->undo_count = 0;
  state->pressed_cell = -1;
  state->elapsed_ms = 0;
  state->clock_started_us = NowUs();
  LayoutBoard(state);
  UpdateSubtitle(state);
  UpdateButtons(state);
  UpdateClock(state);
  Save(state);
}

void StartGenerating(PetdokuState* state, int size) {
  if (state->job != nullptr) {
    return;
  }
  auto* job = new (std::nothrow) GeneratorJob();
  if (job == nullptr) {
    return;
  }
  const int index = SizeIndex(size);
  if (state->next_number[index] == 0) {
    state->next_number[index] = 1;
  }
  job->size = size;
  job->number = state->next_number[index]++;
  if (xTaskCreate(GeneratorTask, "petdoku_gen", kGeneratorStackBytes, job,
          kGeneratorPriority, nullptr) != pdPASS) {
    delete job;
    return;
  }
  PauseClock(state);
  state->job = job;
  state->has_puzzle = false;
  state->pressed_cell = -1;
  LayoutBoard(state);
  UpdateSubtitle(state);
  UpdateClock(state);
}

void GeneratorPollCallback(lv_timer_t* timer) {
  auto* state = static_cast<PetdokuState*>(lv_timer_get_user_data(timer));
  if (state == nullptr || state->job == nullptr || !state->job->done.load()) {
    return;
  }
  GeneratorJob* job = state->job;
  state->job = nullptr;
  if (job->ok) {
    ApplyPuzzle(state, job->puzzle, job->number);
  } else {
    // Practically never happens; the next number is another seed.
    StartGenerating(state, job->size);
  }
  ReleaseJob(job);
}

void ClockTimerCallback(lv_timer_t* timer) {
  auto* state = static_cast<PetdokuState*>(lv_timer_get_user_data(timer));
  if (state == nullptr) {
    return;
  }
  UpdateClock(state);
  if (state->armed_label != nullptr && NowUs() > state->armed_until_us) {
    UpdateButtons(state);
  }
  state->since_save_ms += kClockPeriodMs;
  if (state->dirty && state->since_save_ms >= kSavePeriodMs) {
    Save(state);
  }
}

/**
 * @brief First tap on a button that throws progress away turns it into a
 *        question; only a second tap within a few seconds acts.
 * @return true when the action should go ahead.
 */
bool Confirmed(PetdokuState* state, lv_obj_t* label, const char* question) {
  const bool progress = state->has_puzzle && !state->solved &&
      std::any_of(state->marks,
          state->marks + state->puzzle.size * state->puzzle.size,
          [](PetdokuMark mark) { return mark != PetdokuMark::kEmpty; });
  if (!progress) {
    return true;
  }
  if (state->armed_label == label && NowUs() <= state->armed_until_us) {
    return true;
  }
  UpdateButtons(state);
  lv_label_set_text(label, question);
  state->armed_label = label;
  state->armed_until_us = NowUs() + kConfirmWindowMs * 1000LL;
  PlayUiHapticEffect(HapticEffect::kSelection);
  return false;
}

void UndoClickedCallback(lv_event_t* event) {
  auto* state = static_cast<PetdokuState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->undo_count == 0 || state->solved ||
      !state->has_puzzle) {
    return;
  }
  // A whole tap-and-drag goes back in one step.
  const uint16_t gesture = state->undo[state->undo_count - 1].gesture;
  while (state->undo_count > 0 &&
         state->undo[state->undo_count - 1].gesture == gesture) {
    const UndoEntry& entry = state->undo[--state->undo_count];
    state->marks[entry.cell] = entry.previous;
  }
  Evaluate(state);
  state->dirty = true;
  lv_obj_invalidate(state->board);
  PlayUiHapticEffect(HapticEffect::kPuzzleTap);
}

void ClearClickedCallback(lv_event_t* event) {
  auto* state = static_cast<PetdokuState*>(lv_event_get_user_data(event));
  if (state == nullptr || !state->has_puzzle || state->solved ||
      !Confirmed(state, state->clear_label, "Clear?")) {
    return;
  }
  std::memset(state->marks, 0, sizeof(state->marks));
  state->undo_count = 0;
  Evaluate(state);
  state->dirty = true;
  UpdateButtons(state);
  lv_obj_invalidate(state->board);
  PlayUiHapticEffect(HapticEffect::kImpactLight);
}

void SizeClickedCallback(lv_event_t* event) {
  auto* state = static_cast<PetdokuState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->job != nullptr) {
    return;
  }
  const int current = state->has_puzzle ? state->puzzle.size : kDefaultSize;
  // While a question is pending, further taps step on through the sizes.
  const int base = state->armed_label == state->size_label &&
                           NowUs() <= state->armed_until_us
                       ? state->armed_size
                       : current;
  const int next =
      base >= games::kPetdokuMaxSize ? games::kPetdokuMinSize : base + 1;
  const bool progress = state->has_puzzle && !state->solved &&
      std::any_of(state->marks,
          state->marks + state->puzzle.size * state->puzzle.size,
          [](PetdokuMark mark) { return mark != PetdokuMark::kEmpty; });
  if (progress) {
    // Show the size a second tap of "New" would start, without losing the
    // board yet.
    UpdateButtons(state);
    char text[32];
    std::snprintf(text, sizeof(text), "%d \xC3\x97 %d", next, next);
    lv_label_set_text(state->size_label, text);
    lv_label_set_text(state->new_label, "Start");
    state->armed_label = state->size_label;
    state->armed_size = next;
    state->armed_until_us = NowUs() + kConfirmWindowMs * 1000LL;
    PlayUiHapticEffect(HapticEffect::kSelection);
    return;
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  StartGenerating(state, next);
  UpdateButtons(state);
}

void NewClickedCallback(lv_event_t* event) {
  auto* state = static_cast<PetdokuState*>(lv_event_get_user_data(event));
  if (state == nullptr || state->job != nullptr) {
    return;
  }
  int size = state->has_puzzle ? state->puzzle.size : kDefaultSize;
  if (state->armed_label == state->size_label &&
      NowUs() <= state->armed_until_us) {
    size = state->armed_size;
  } else if (!Confirmed(state, state->new_label, "Sure?")) {
    return;
  }
  PlayUiHapticEffect(HapticEffect::kImpactLight);
  StartGenerating(state, size);
  UpdateButtons(state);
}

lv_obj_t* CreatePill(PetdokuState* state, lv_obj_t* parent, int x, int width,
    const char* text, lv_event_cb_t callback, lv_obj_t** label_out) {
  const theme::ThemeColors& colors = theme::ActiveThemeColors();
  lv_obj_t* pill = lv_obj_create(parent);
  if (pill == nullptr) {
    return nullptr;
  }
  lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(pill, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_size(pill, width, kControlsHeight);
  lv_obj_set_pos(
      pill, x, state->height - kControlsBottomMargin - kControlsHeight);
  lv_obj_set_style_radius(pill, kControlsHeight / 2, 0);
  lv_obj_set_style_border_width(pill, 0, 0);
  lv_obj_set_style_pad_all(pill, 0, 0);
  lv_obj_set_style_bg_color(
      pill, lv_color_hex(colors.surface_container_high), 0);
  lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(
      pill, lv_color_hex(colors.surface_container_highest), LV_STATE_PRESSED);
  lv_obj_add_event_cb(pill, callback, LV_EVENT_CLICKED, state);
  lv_obj_t* label = lv_label_create(pill);
  if (label != nullptr) {
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(colors.on_surface), 0);
    lv_obj_set_style_text_font(label, &lvgl_font_google_sans_flex_24, 0);
    lv_obj_center(label);
  }
  if (label_out != nullptr) {
    *label_out = label;
  }
  return pill;
}

void ViewDeletedCallback(lv_event_t* event) {
  auto* state = static_cast<PetdokuState*>(lv_event_get_user_data(event));
  if (state == nullptr) {
    return;
  }
  if (state->clock_timer != nullptr) {
    lv_timer_delete(state->clock_timer);
  }
  if (state->generator_timer != nullptr) {
    lv_timer_delete(state->generator_timer);
  }
  if (state->has_puzzle) {
    PauseClock(state);
    Save(state);
  }
  ReleaseJob(state->job);
  delete state;
}

}  // namespace

lv_obj_t* CreatePetdokuView(lv_obj_t* parent, const app::AppEntry& app_entry,
    const AppViewConfig& config) {
  (void)app_entry;
  if (parent == nullptr || config.width <= 0 || config.height <= 0) {
    return nullptr;
  }
  const theme::ThemeColors& colors = theme::ActiveThemeColors();
  lv_obj_t* root = lv_obj_create(parent);
  if (root == nullptr) {
    return nullptr;
  }
  lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(root, config.width, config.height);
  lv_obj_align(root, LV_ALIGN_CENTER, 0, 0);
  lv_obj_set_style_radius(root, 0, 0);
  lv_obj_set_style_border_width(root, 0, 0);
  lv_obj_set_style_pad_all(root, 0, 0);
  lv_obj_set_style_bg_color(root, lv_color_hex(colors.surface), 0);
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

  auto* state = new (std::nothrow) PetdokuState();
  if (state == nullptr) {
    lv_obj_delete(root);
    return nullptr;
  }
  state->root = root;
  state->width = config.width;
  state->height = config.height;
  lv_obj_add_event_cb(root, ViewDeletedCallback, LV_EVENT_DELETE, state);

  lv_obj_t* title = lv_label_create(root);
  if (title != nullptr) {
    lv_label_set_text(title, "Petdoku");
    lv_obj_set_style_text_color(title, lv_color_hex(colors.on_surface), 0);
    lv_obj_set_style_text_font(title, &lvgl_font_google_sans_flex_36, 0);
    lv_obj_set_pos(title, kSidePadding + 4, kTitleTop);
  }
  state->clock_label = lv_label_create(root);
  if (state->clock_label != nullptr) {
    lv_obj_set_style_text_color(
        state->clock_label, lv_color_hex(colors.on_surface_variant), 0);
    lv_obj_set_style_text_font(
        state->clock_label, &lvgl_font_google_sans_flex_32, 0);
    lv_obj_align(state->clock_label, LV_ALIGN_TOP_RIGHT, -kSidePadding - 4,
        kTitleTop + 2);
  }
  state->subtitle = lv_label_create(root);
  if (state->subtitle != nullptr) {
    lv_obj_set_style_text_font(
        state->subtitle, &lvgl_font_google_sans_flex_22, 0);
    lv_obj_set_pos(state->subtitle, kSidePadding + 4, kSubtitleTop);
  }

  state->board = lv_obj_create(root);
  if (state->board == nullptr) {
    lv_obj_delete(root);
    return nullptr;
  }
  lv_obj_remove_flag(state->board, LV_OBJ_FLAG_SCROLLABLE);
  // Drags on the board mark cells; they must not reach the page as gestures.
  lv_obj_remove_flag(state->board, LV_OBJ_FLAG_GESTURE_BUBBLE);
  lv_obj_add_flag(state->board, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_opa(state->board, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(state->board, 0, 0);
  lv_obj_set_style_pad_all(state->board, 0, 0);
  lv_obj_add_event_cb(
      state->board, BoardDrawCallback, LV_EVENT_DRAW_MAIN, state);
  lv_obj_add_event_cb(
      state->board, BoardInputCallback, LV_EVENT_PRESSED, state);
  lv_obj_add_event_cb(
      state->board, BoardInputCallback, LV_EVENT_PRESSING, state);
  lv_obj_add_event_cb(
      state->board, BoardInputCallback, LV_EVENT_RELEASED, state);
  lv_obj_add_event_cb(
      state->board, BoardInputCallback, LV_EVENT_PRESS_LOST, state);

  const int pill_width =
      (config.width - 2 * kSidePadding - 3 * kControlsGap) / 4;
  int x = kSidePadding;
  state->undo_button =
      CreatePill(state, root, x, pill_width, "Undo", UndoClickedCallback,
          nullptr);
  x += pill_width + kControlsGap;
  CreatePill(state, root, x, pill_width, "Clear", ClearClickedCallback,
      &state->clear_label);
  x += pill_width + kControlsGap;
  CreatePill(state, root, x, pill_width, "8 \xC3\x97 8", SizeClickedCallback,
      &state->size_label);
  x += pill_width + kControlsGap;
  CreatePill(state, root, x, pill_width, "New", NewClickedCallback,
      &state->new_label);

  state->clock_timer =
      lv_timer_create(ClockTimerCallback, kClockPeriodMs, state);
  state->generator_timer =
      lv_timer_create(GeneratorPollCallback, kGeneratorPollMs, state);

  if (Load(state)) {
    games::CheckPetdoku(state->puzzle, state->marks, &state->check);
    if (!state->solved) {
      state->clock_started_us = NowUs();
    }
    LayoutBoard(state);
    UpdateSubtitle(state);
    UpdateButtons(state);
    UpdateClock(state);
  } else {
    StartGenerating(state, kDefaultSize);
    UpdateButtons(state);
  }
  return root;
}

}  // namespace lilygo_box::ui
