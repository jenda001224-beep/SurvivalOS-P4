/*
 * @Description: Petdoku puzzle rules, logic solver and generator
 * @License: GPL 3.0
 */
#include "games/petdoku_engine.h"

#include <cstdlib>
#include <cstring>

namespace lilygo_box::games {
namespace {

int Popcount(uint32_t value) { return __builtin_popcount(value); }

bool Touching(int row_a, int col_a, int row_b, int col_b) {
  return std::abs(row_a - row_b) <= 1 && std::abs(col_a - col_b) <= 1;
}

/**
 * @brief Small deterministic generator, so a seed always gives the same
 *        puzzle on the device and in the host test.
 */
class Random {
 public:
  explicit Random(uint32_t seed) : state_(seed != 0 ? seed : 0x9E3779B9U) {}

  uint32_t Next() {
    state_ ^= state_ << 13;
    state_ ^= state_ >> 17;
    state_ ^= state_ << 5;
    return state_;
  }

  int Below(int bound) { return static_cast<int>(Next() % bound); }

 private:
  uint32_t state_;
};

/**
 * @brief Candidate bookkeeping for the deduction solver.
 */
class Solver {
 public:
  explicit Solver(const PetdokuPuzzle& puzzle)
      : puzzle_(puzzle), size_(puzzle.size) {
    for (int cell = 0; cell < size_ * size_; ++cell) {
      candidate_[cell] = true;
    }
  }

  bool Run(uint8_t* solution, int* difficulty) {
    int hard_steps = 0;
    while (true) {
      const Progress singles = PlaceSingles();
      if (singles == Progress::kContradiction) {
        return false;
      }
      if (placed_count_ == size_) {
        break;
      }
      if (singles == Progress::kChanged) {
        continue;
      }
      if (ConfineSets()) {
        ++hard_steps;
        continue;
      }
      if (RuleOutBlockers()) {
        hard_steps += 2;
        continue;
      }
      return false;  // Stuck: this puzzle would need guessing.
    }
    if (solution != nullptr) {
      std::memcpy(solution, placed_column_, size_);
    }
    if (difficulty != nullptr) {
      *difficulty = hard_steps;
    }
    return true;
  }

 private:
  enum class Progress : uint8_t { kNone, kChanged, kContradiction };

  int Region(int row, int col) const {
    return puzzle_.region[row * size_ + col];
  }

  void Remove(int row, int col) { candidate_[row * size_ + col] = false; }

  bool IsCandidate(int row, int col) const {
    return candidate_[row * size_ + col];
  }

  void Place(int row, int col) {
    const int region = Region(row, col);
    row_done_ |= 1U << row;
    col_done_ |= 1U << col;
    region_done_ |= 1U << region;
    placed_column_[row] = static_cast<uint8_t>(col);
    ++placed_count_;
    for (int r = 0; r < size_; ++r) {
      for (int c = 0; c < size_; ++c) {
        if (r == row || c == col || Region(r, c) == region ||
            Touching(r, c, row, col)) {
          Remove(r, c);
        }
      }
    }
  }

  // Bit masks of where the candidates of a unit lie.
  uint32_t RegionRows(int region) const {
    uint32_t rows = 0;
    for (int r = 0; r < size_; ++r) {
      for (int c = 0; c < size_; ++c) {
        if (IsCandidate(r, c) && Region(r, c) == region) {
          rows |= 1U << r;
        }
      }
    }
    return rows;
  }

  uint32_t RegionCols(int region) const {
    uint32_t cols = 0;
    for (int r = 0; r < size_; ++r) {
      for (int c = 0; c < size_; ++c) {
        if (IsCandidate(r, c) && Region(r, c) == region) {
          cols |= 1U << c;
        }
      }
    }
    return cols;
  }

  /**
   * @brief Places a pet where a row, column or region has a single possible
   *        cell left; reports a unit with none as a contradiction.
   */
  Progress PlaceSingles() {
    for (int unit = 0; unit < 3 * size_; ++unit) {
      const int kind = unit / size_;
      const int index = unit % size_;
      const uint32_t done = kind == 0 ? row_done_
                            : kind == 1 ? col_done_
                                        : region_done_;
      if ((done & (1U << index)) != 0) {
        continue;
      }
      int count = 0;
      int last_row = -1;
      int last_col = -1;
      for (int r = 0; r < size_; ++r) {
        for (int c = 0; c < size_; ++c) {
          const bool in_unit = kind == 0   ? r == index
                               : kind == 1 ? c == index
                                           : Region(r, c) == index;
          if (in_unit && IsCandidate(r, c)) {
            ++count;
            last_row = r;
            last_col = c;
          }
        }
      }
      if (count == 0) {
        return Progress::kContradiction;
      }
      if (count == 1) {
        Place(last_row, last_col);
        return Progress::kChanged;
      }
    }
    return Progress::kNone;
  }

  /**
   * @brief k open regions whose cells all lie in k rows (or columns) own
   *        those rows: nothing else in them can be a pet. And the reverse: k
   *        open rows (columns) whose cells all lie in k regions own those
   *        regions. k = 1 is the familiar "region in a single line".
   */
  bool ConfineSets() {
    bool changed = false;
    const uint32_t all = (1U << size_) - 1;
    // Where the candidates of every region, row and column lie.
    uint32_t region_rows[kPetdokuMaxSize] = {};
    uint32_t region_cols[kPetdokuMaxSize] = {};
    uint32_t row_regions[kPetdokuMaxSize] = {};
    uint32_t col_regions[kPetdokuMaxSize] = {};
    for (int r = 0; r < size_; ++r) {
      for (int c = 0; c < size_; ++c) {
        if (IsCandidate(r, c)) {
          const int g = Region(r, c);
          region_rows[g] |= 1U << r;
          region_cols[g] |= 1U << c;
          row_regions[r] |= 1U << g;
          col_regions[c] |= 1U << g;
        }
      }
    }

    const uint32_t open_regions = all & ~region_done_;
    const int open_region_count = Popcount(open_regions);
    for (uint32_t set = open_regions; set != 0;
         set = (set - 1) & open_regions) {
      const int k = Popcount(set);
      if (k == open_region_count) {
        continue;
      }
      uint32_t rows = 0;
      uint32_t cols = 0;
      for (uint32_t bits = set; bits != 0; bits &= bits - 1) {
        const int g = __builtin_ctz(bits);
        rows |= region_rows[g];
        cols |= region_cols[g];
      }
      const bool own_rows = Popcount(rows) == k;
      const bool own_cols = Popcount(cols) == k;
      if (!own_rows && !own_cols) {
        continue;
      }
      for (int r = 0; r < size_; ++r) {
        for (int c = 0; c < size_; ++c) {
          if (IsCandidate(r, c) && (set & (1U << Region(r, c))) == 0 &&
              ((own_rows && (rows & (1U << r)) != 0) ||
                  (own_cols && (cols & (1U << c)) != 0))) {
            Remove(r, c);
            changed = true;
          }
        }
      }
    }

    // Rows and columns confined to as many regions.
    for (int axis = 0; axis < 2; ++axis) {
      const uint32_t open = all & ~(axis == 0 ? row_done_ : col_done_);
      const int open_count = Popcount(open);
      const uint32_t* line_regions = axis == 0 ? row_regions : col_regions;
      for (uint32_t set = open; set != 0; set = (set - 1) & open) {
        const int k = Popcount(set);
        if (k == open_count) {
          continue;
        }
        uint32_t regions = 0;
        for (uint32_t bits = set; bits != 0; bits &= bits - 1) {
          regions |= line_regions[__builtin_ctz(bits)];
        }
        if (Popcount(regions) != k) {
          continue;
        }
        for (int r = 0; r < size_; ++r) {
          for (int c = 0; c < size_; ++c) {
            const int line = axis == 0 ? r : c;
            if (IsCandidate(r, c) && (set & (1U << line)) == 0 &&
                (regions & (1U << Region(r, c))) != 0) {
              Remove(r, c);
              changed = true;
            }
          }
        }
      }
    }
    return changed;
  }

  /**
   * @brief Rules out a cell whose pet would leave another row, column or
   *        region with no possible cell at all.
   */
  bool RuleOutBlockers() {
    bool changed = false;
    for (int row = 0; row < size_; ++row) {
      for (int col = 0; col < size_; ++col) {
        if (!IsCandidate(row, col)) {
          continue;
        }
        const int region = Region(row, col);
        // Count, per unit, the candidates this pet would leave standing.
        int row_left[kPetdokuMaxSize] = {};
        int col_left[kPetdokuMaxSize] = {};
        int region_left[kPetdokuMaxSize] = {};
        for (int r = 0; r < size_; ++r) {
          for (int c = 0; c < size_; ++c) {
            if (!IsCandidate(r, c) || r == row || c == col ||
                Region(r, c) == region || Touching(r, c, row, col)) {
              continue;
            }
            ++row_left[r];
            ++col_left[c];
            ++region_left[Region(r, c)];
          }
        }
        bool blocks = false;
        for (int i = 0; i < size_ && !blocks; ++i) {
          blocks = (i != row && (row_done_ & (1U << i)) == 0 &&
                       row_left[i] == 0) ||
                   (i != col && (col_done_ & (1U << i)) == 0 &&
                       col_left[i] == 0) ||
                   (i != region && (region_done_ & (1U << i)) == 0 &&
                       region_left[i] == 0);
        }
        if (blocks) {
          Remove(row, col);
          changed = true;
        }
      }
    }
    return changed;
  }

  const PetdokuPuzzle& puzzle_;
  const int size_;
  bool candidate_[kPetdokuMaxCells] = {};
  uint8_t placed_column_[kPetdokuMaxSize] = {};
  int placed_count_ = 0;
  uint32_t row_done_ = 0;
  uint32_t col_done_ = 0;
  uint32_t region_done_ = 0;
};

/**
 * @brief One pet per row and column, none touching: a random permutation in
 *        which neighbouring rows differ by more than one column.
 */
bool PlaceSolution(int size, Random* random, uint8_t* columns, int row,
    uint32_t used) {
  if (row == size) {
    return true;
  }
  int order[kPetdokuMaxSize];
  for (int c = 0; c < size; ++c) {
    order[c] = c;
  }
  for (int i = size - 1; i > 0; --i) {
    const int j = random->Below(i + 1);
    const int swap = order[i];
    order[i] = order[j];
    order[j] = swap;
  }
  for (int i = 0; i < size; ++i) {
    const int col = order[i];
    if ((used & (1U << col)) != 0 ||
        (row > 0 && std::abs(col - columns[row - 1]) <= 1)) {
      continue;
    }
    columns[row] = static_cast<uint8_t>(col);
    if (PlaceSolution(size, random, columns, row + 1, used | (1U << col))) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Grows one region from every pet until the grid is covered: each step
 *        hands a random free cell next to a region to one of its neighbours.
 */
void GrowRegions(
    int size, Random* random, const uint8_t* columns, uint8_t* region) {
  const int cells = size * size;
  int8_t owner[kPetdokuMaxCells];
  std::memset(owner, -1, sizeof(owner));
  for (int r = 0; r < size; ++r) {
    owner[r * size + columns[r]] = static_cast<int8_t>(r);
  }
  int free_cells = cells - size;
  static const int kSteps[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  while (free_cells > 0) {
    // Collect the free cells on the boundary and pick one.
    int boundary[kPetdokuMaxCells];
    int boundary_count = 0;
    for (int cell = 0; cell < cells; ++cell) {
      if (owner[cell] >= 0) {
        continue;
      }
      const int r = cell / size;
      const int c = cell % size;
      for (const auto& step : kSteps) {
        const int nr = r + step[0];
        const int nc = c + step[1];
        if (nr >= 0 && nr < size && nc >= 0 && nc < size &&
            owner[nr * size + nc] >= 0) {
          boundary[boundary_count++] = cell;
          break;
        }
      }
    }
    const int cell = boundary[random->Below(boundary_count)];
    const int r = cell / size;
    const int c = cell % size;
    int8_t neighbours[4];
    int neighbour_count = 0;
    for (const auto& step : kSteps) {
      const int nr = r + step[0];
      const int nc = c + step[1];
      if (nr >= 0 && nr < size && nc >= 0 && nc < size &&
          owner[nr * size + nc] >= 0) {
        neighbours[neighbour_count++] = owner[nr * size + nc];
      }
    }
    owner[cell] = neighbours[random->Below(neighbour_count)];
    --free_cells;
  }
  // Region numbers in a random order, so colours do not follow the rows.
  uint8_t relabel[kPetdokuMaxSize];
  for (int g = 0; g < size; ++g) {
    relabel[g] = static_cast<uint8_t>(g);
  }
  for (int i = size - 1; i > 0; --i) {
    const int j = random->Below(i + 1);
    const uint8_t swap = relabel[i];
    relabel[i] = relabel[j];
    relabel[j] = swap;
  }
  for (int cell = 0; cell < cells; ++cell) {
    region[cell] = relabel[owner[cell]];
  }
}

/**
 * @brief Search for a solution other than `avoid`, row by row, dropping a
 *        branch as soon as some unused region has no free cell left below.
 */
class OtherSolutionSearch {
 public:
  OtherSolutionSearch(const PetdokuPuzzle& puzzle, const uint8_t* avoid)
      : puzzle_(puzzle), size_(puzzle.size), avoid_(avoid) {
    for (int r = 0; r < size_; ++r) {
      for (int c = 0; c < size_; ++c) {
        region_cols_[puzzle.region[r * size_ + c]][r] |= 1U << c;
      }
    }
  }

  /**
   * @return true with the solution in `found`; false when there is none or
   *         the search ran out of `budget` steps.
   */
  bool Find(int budget, uint8_t* found) {
    budget_ = budget;
    found_ = found;
    return Search(0, 0, 0, false);
  }

  /** @brief Whether the last Find gave up before searching everything. */
  bool OutOfBudget() const { return budget_ < 0; }

 private:
  bool Search(int row, uint32_t used_cols, uint32_t used_regions,
      bool differs) {
    if (--budget_ < 0) {
      return false;
    }
    if (row == size_) {
      if (differs) {
        std::memcpy(found_, columns_, size_);
      }
      return differs;
    }
    for (int col = 0; col < size_; ++col) {
      const int region = puzzle_.region[row * size_ + col];
      if ((used_cols & (1U << col)) != 0 ||
          (used_regions & (1U << region)) != 0 ||
          (row > 0 && std::abs(col - columns_[row - 1]) <= 1)) {
        continue;
      }
      const uint32_t cols = used_cols | (1U << col);
      const uint32_t regions = used_regions | (1U << region);
      if (!RegionsReachable(row, col, cols, regions)) {
        continue;
      }
      columns_[row] = static_cast<uint8_t>(col);
      if (Search(row + 1, cols, regions, differs || col != avoid_[row])) {
        return true;
      }
    }
    return false;
  }

  // Every unused region still needs a free cell in one of the rows below.
  bool RegionsReachable(
      int row, int col, uint32_t used_cols, uint32_t used_regions) const {
    const uint32_t all = (1U << size_) - 1;
    const uint32_t touching =
        ((1U << col) | (col > 0 ? 1U << (col - 1) : 0) | (1U << (col + 1))) &
        all;
    for (int g = 0; g < size_; ++g) {
      if ((used_regions & (1U << g)) != 0) {
        continue;
      }
      bool reachable = false;
      for (int r = row + 1; r < size_ && !reachable; ++r) {
        const uint32_t blocked = used_cols | (r == row + 1 ? touching : 0);
        reachable = (region_cols_[g][r] & ~blocked) != 0;
      }
      if (!reachable) {
        return false;
      }
    }
    return true;
  }

  const PetdokuPuzzle& puzzle_;
  const int size_;
  const uint8_t* avoid_;
  uint16_t region_cols_[kPetdokuMaxSize][kPetdokuMaxSize] = {};
  uint8_t columns_[kPetdokuMaxSize] = {};
  uint8_t* found_ = nullptr;
  int budget_ = 0;
};

/**
 * @brief Whether a region stays in one piece without the given cell.
 */
bool ConnectedWithout(const PetdokuPuzzle& puzzle, int region, int removed) {
  const int size = puzzle.size;
  bool seen[kPetdokuMaxCells] = {};
  int stack[kPetdokuMaxCells];
  int top = 0;
  int total = 0;
  for (int cell = 0; cell < size * size; ++cell) {
    if (cell != removed && puzzle.region[cell] == region) {
      ++total;
      if (top == 0 && total == 1) {
        stack[top++] = cell;
        seen[cell] = true;
      }
    }
  }
  int reached = 0;
  while (top > 0) {
    const int cell = stack[--top];
    ++reached;
    const int r = cell / size;
    const int c = cell % size;
    const int next[4] = {r > 0 ? cell - size : -1,
        r < size - 1 ? cell + size : -1, c > 0 ? cell - 1 : -1,
        c < size - 1 ? cell + 1 : -1};
    for (const int neighbour : next) {
      if (neighbour >= 0 && neighbour != removed && !seen[neighbour] &&
          puzzle.region[neighbour] == region) {
        seen[neighbour] = true;
        stack[top++] = neighbour;
      }
    }
  }
  return total > 0 && reached == total;
}

/**
 * @brief Reshapes one region so that `other` stops being a solution.
 *
 * `other` has a pet in some cell the planted solution leaves empty. Handing
 * that cell to a neighbouring region puts two of its pets in that region,
 * while the planted solution is untouched: the cell was not one of its pets,
 * and every region still holds exactly one of them.
 * @return false when no cell of `other` can be moved without splitting its
 *         region.
 */
bool BreakOtherSolution(
    PetdokuPuzzle* puzzle, const uint8_t* other, Random* random) {
  const int size = puzzle->size;
  int rows[kPetdokuMaxSize];
  int row_count = 0;
  for (int r = 0; r < size; ++r) {
    if (other[r] != puzzle->solution[r]) {
      rows[row_count++] = r;
    }
  }
  for (int attempt = 0; attempt < row_count; ++attempt) {
    const int pick = attempt + random->Below(row_count - attempt);
    const int swap = rows[attempt];
    rows[attempt] = rows[pick];
    rows[pick] = swap;
    const int r = rows[attempt];
    const int c = other[r];
    const int cell = r * size + c;
    const int from = puzzle->region[cell];
    if (!ConnectedWithout(*puzzle, from, cell)) {
      continue;
    }
    int targets[4];
    int target_count = 0;
    const int next[4] = {r > 0 ? cell - size : -1,
        r < size - 1 ? cell + size : -1, c > 0 ? cell - 1 : -1,
        c < size - 1 ? cell + 1 : -1};
    for (const int neighbour : next) {
      if (neighbour >= 0 && puzzle->region[neighbour] != from) {
        targets[target_count++] = puzzle->region[neighbour];
      }
    }
    if (target_count > 0) {
      puzzle->region[cell] =
          static_cast<uint8_t>(targets[random->Below(target_count)]);
      return true;
    }
  }
  return false;
}

}  // namespace

void CheckPetdoku(const PetdokuPuzzle& puzzle, const PetdokuMark* marks,
    PetdokuCheck* check) {
  *check = PetdokuCheck();
  const int size = puzzle.size;
  int pets[kPetdokuMaxCells];
  int pet_count = 0;
  for (int cell = 0; cell < size * size; ++cell) {
    if (marks[cell] == PetdokuMark::kPet) {
      pets[pet_count++] = cell;
    }
  }
  for (int i = 0; i < pet_count; ++i) {
    for (int j = i + 1; j < pet_count; ++j) {
      const int ra = pets[i] / size;
      const int ca = pets[i] % size;
      const int rb = pets[j] / size;
      const int cb = pets[j] % size;
      if (ra == rb || ca == cb ||
          puzzle.region[pets[i]] == puzzle.region[pets[j]] ||
          Touching(ra, ca, rb, cb)) {
        check->conflict[pets[i]] = true;
        check->conflict[pets[j]] = true;
        check->has_conflict = true;
      }
    }
  }
  check->pet_count = pet_count;
  // N pets with no two sharing a row, column or region means one in each.
  check->solved = pet_count == size && !check->has_conflict;
}

bool SolvePetdoku(
    const PetdokuPuzzle& puzzle, uint8_t* solution, int* difficulty) {
  if (puzzle.size < kPetdokuMinSize || puzzle.size > kPetdokuMaxSize) {
    return false;
  }
  Solver solver(puzzle);
  return solver.Run(solution, difficulty);
}

bool GeneratePetdoku(
    int size, uint32_t seed, int max_attempts, PetdokuPuzzle* puzzle) {
  // Search steps allowed when looking for a second solution.
  constexpr int kSearchBudget = 20000;
  if (size < kPetdokuMinSize || size > kPetdokuMaxSize || puzzle == nullptr) {
    return false;
  }
  Random random(seed * 2654435761U + static_cast<uint32_t>(size) * 40503U);
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    PetdokuPuzzle candidate;
    candidate.size = size;
    if (!PlaceSolution(size, &random, candidate.solution, 0, 0)) {
      continue;
    }
    GrowRegions(size, &random, candidate.solution, candidate.region);
    // Reshape until no other solution is left, then make sure the one that
    // remains can be found by deduction rather than guessing.
    for (int round = 0; round < 4 * size * size; ++round) {
      uint8_t other[kPetdokuMaxSize];
      OtherSolutionSearch search(candidate, candidate.solution);
      if (search.Find(kSearchBudget, other)) {
        if (!BreakOtherSolution(&candidate, other, &random)) {
          break;
        }
        continue;
      }
      if (search.OutOfBudget()) {
        break;
      }
      uint8_t solved[kPetdokuMaxSize];
      if (SolvePetdoku(candidate, solved, nullptr) &&
          std::memcmp(solved, candidate.solution, size) == 0) {
        *puzzle = candidate;
        return true;
      }
      break;
    }
  }
  return false;
}

}  // namespace lilygo_box::games
