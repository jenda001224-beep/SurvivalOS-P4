/*
 * @Description: Petdoku puzzle rules, logic solver and generator
 * @License: GPL 3.0
 */
#pragma once

#include <cstdint>

namespace lilygo_box::games {

// Petdoku: an N x N grid split into N regions. A solution puts N pets on it,
// exactly one in every row, every column and every region, and no two pets in
// touching cells, diagonals included. Every generated puzzle has exactly one
// solution, and it can be reached by deduction alone (see SolvePetdoku).
//
// Plain C++ without LVGL, so the same code runs in the host test
// (hosttest/petdoku/).

inline constexpr int kPetdokuMinSize = 6;
inline constexpr int kPetdokuMaxSize = 10;
inline constexpr int kPetdokuMaxCells = kPetdokuMaxSize * kPetdokuMaxSize;

enum class PetdokuMark : uint8_t {
  kEmpty,
  // The player ruled the cell out.
  kExcluded,
  kPet,
};

struct PetdokuPuzzle {
  int size = 0;
  // Region (0 .. size-1) of every cell, row by row.
  uint8_t region[kPetdokuMaxCells] = {};
  // Column of the pet in every row.
  uint8_t solution[kPetdokuMaxSize] = {};
};

struct PetdokuCheck {
  int pet_count = 0;
  // Pets that break a rule: share a row, column or region with another pet,
  // or touch one. Indexed like PetdokuPuzzle::region.
  bool conflict[kPetdokuMaxCells] = {};
  bool has_conflict = false;
  // Exactly one pet in every row, column and region and none touching.
  bool solved = false;
};

/**
 * @brief Checks the player's marks against the rules (not the stored solution:
 *        any placement that satisfies the rules counts).
 * @param marks size*size marks, row by row
 */
void CheckPetdoku(const PetdokuPuzzle& puzzle, const PetdokuMark* marks,
    PetdokuCheck* check);

/**
 * @brief Solves a puzzle by deduction only, without guessing.
 *
 * Each step is one a person can make: a row, column or region with a single
 * possible cell; ruling out the cells around and in line with a pet; regions
 * confined to as many rows (or columns) as there are regions, and the reverse;
 * and ruling out a cell whose pet would leave some row, column or region
 * without any possible cell.
 * @param solution Receives the pet column of every row when solved.
 * @param difficulty Receives a rough difficulty: how many of the harder steps
 *        were needed. May be nullptr.
 * @return true when the steps lead to a complete solution, which is then the
 *         only one.
 */
bool SolvePetdoku(
    const PetdokuPuzzle& puzzle, uint8_t* solution, int* difficulty);

/**
 * @brief Generates a puzzle with a unique solution reachable by deduction.
 * @param size Grid size, kPetdokuMinSize .. kPetdokuMaxSize
 * @param seed Any number; the same seed gives the same puzzle.
 * @param max_attempts Region layouts to try before giving up.
 * @return false when no layout passed within max_attempts.
 */
bool GeneratePetdoku(
    int size, uint32_t seed, int max_attempts, PetdokuPuzzle* puzzle);

}  // namespace lilygo_box::games
