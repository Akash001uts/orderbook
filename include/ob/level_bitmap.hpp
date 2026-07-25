#pragma once

#include <array>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace ob {

// Hierarchical occupancy bitmap over the price levels of one side.
//
// Three tiers. The bottom tier has one bit per level. The middle tier has one bit
// per bottom-tier word, set when that word is non-zero. The top tier is a single
// word with one bit per middle-tier word. Finding the best price is then a
// countr_zero on the top word to pick a middle word, a countr_zero on that to
// pick a bottom word, and a countr_zero on that to pick the level: three
// dependent loads and three single-cycle instructions, independent of how far
// apart the occupied levels are.
//
// Alternatives considered:
//
//   Scan the level array outward from the last known best. O(distance), and the
//   distance is unbounded: one large cancel at the touch can leave the next
//   occupied level thousands of ticks away, which turns a cancel into a scan of
//   tens of kilobytes at exactly the moment the book is busiest.
//
//   Cache best_bid and best_ask as plain values. O(1) to read, but repairing them
//   after the best level empties needs the scan above, so it moves the cost rather
//   than removing it, and it adds a second source of truth that can disagree with
//   the levels.
//
//   A tree or a heap keyed by price. O(log n) with pointer chasing and allocation,
//   which is the design this project exists to beat.
//
// Invariant, and the sharpest edge in the whole book: a bit is set if and only if
// the corresponding level has a non-zero order count. Occupancy changes only when
// a level goes from empty to occupied or back, never when a quantity changes, so
// there are exactly two call sites per side that may touch this structure. If a
// third appears, this invariant is what it will break, and the symptom will be
// best_bid reporting a price with no liquidity behind it.
template <std::size_t Levels>
class LevelBitmap {
  static_assert(Levels % 64 == 0, "level count must be a whole number of bottom-tier words");
  static_assert(Levels >= 4096, "a two-tier summary needs at least 64 bottom-tier words");

  static constexpr std::size_t BOTTOM_WORDS = Levels / 64;
  static constexpr std::size_t MIDDLE_WORDS = (BOTTOM_WORDS + 63) / 64;

  static_assert(MIDDLE_WORDS <= 64, "a fourth tier would be needed above this level count");

 public:
  static constexpr std::size_t LEVEL_COUNT = Levels;

  // Returned by the query functions when the side is empty. Levels is one past
  // the last valid index, so it can never collide with a real answer.
  static constexpr std::size_t NONE = Levels;

  void set(std::size_t level) noexcept {
    assert(level < Levels);
    const std::size_t bottom = level >> 6U;
    const std::size_t middle = bottom >> 6U;

    bottom_[bottom] |= bit(level);
    middle_[middle] |= bit(bottom);
    top_ |= bit(middle);
  }

  // The early returns are the reason occupancy is only touched on transitions: a
  // clear that does not empty its word costs one load, one and, one store, and
  // never reads the tiers above.
  void clear(std::size_t level) noexcept {
    assert(level < Levels);
    const std::size_t bottom = level >> 6U;

    bottom_[bottom] &= ~bit(level);
    if (bottom_[bottom] != 0U) {
      return;
    }

    const std::size_t middle = bottom >> 6U;
    middle_[middle] &= ~bit(bottom);
    if (middle_[middle] != 0U) {
      return;
    }

    top_ &= ~bit(middle);
  }

  [[nodiscard]] bool test(std::size_t level) const noexcept {
    assert(level < Levels);
    return (bottom_[level >> 6U] & bit(level)) != 0U;
  }

  [[nodiscard]] bool empty() const noexcept { return top_ == 0U; }

  // Lowest occupied level, which is the best ask on the sell side.
  //
  // The two range checks are bounds enforcement, not dead code. countr_zero and
  // bit_width can each return up to 63, and the invariant that stops the derived
  // indices exceeding their arrays is that the top word never carries a bit at or
  // above MIDDLE_WORDS. That invariant is real but the optimiser cannot see it:
  // GCC 13 at -O3 concluded the bottom tier could be indexed at 64 on a 64 word
  // array and refused to compile under -Werror=array-bounds. Stating the bound
  // explicitly is both the fix and the documentation. Neither branch is ever
  // taken, and where MIDDLE_WORDS is 64 the first folds away entirely.
  [[nodiscard]] std::size_t lowest_set() const noexcept {
    if (top_ == 0U) {
      return NONE;
    }

    const auto middle = static_cast<std::size_t>(std::countr_zero(top_));
    if (middle >= MIDDLE_WORDS) {
      return NONE;
    }

    const std::size_t bottom =
        (middle << 6U) + static_cast<std::size_t>(std::countr_zero(middle_[middle]));
    if (bottom >= BOTTOM_WORDS) {
      return NONE;
    }

    return (bottom << 6U) + static_cast<std::size_t>(std::countr_zero(bottom_[bottom]));
  }

  // Highest occupied level, which is the best bid on the buy side. Same bounds
  // reasoning as lowest_set.
  [[nodiscard]] std::size_t highest_set() const noexcept {
    if (top_ == 0U) {
      return NONE;
    }

    const std::size_t middle = highest_bit(top_);
    if (middle >= MIDDLE_WORDS) {
      return NONE;
    }

    const std::size_t bottom = (middle << 6U) + highest_bit(middle_[middle]);
    if (bottom >= BOTTOM_WORDS) {
      return NONE;
    }

    return (bottom << 6U) + highest_bit(bottom_[bottom]);
  }

  // Lowest occupied level at or above `from`.
  //
  // Not a hot path operation. It walks bottom-tier words linearly, which is up to
  // 1024 loads at the default band size, and it exists for band rebasing and for
  // tests that need to enumerate occupancy. Matching walks levels by decrementing
  // or incrementing a known-good index instead, which needs no search.
  [[nodiscard]] std::size_t next_set(std::size_t from) const noexcept {
    if (from >= Levels) {
      return NONE;
    }

    std::size_t bottom = from >> 6U;
    std::uint64_t word = bottom_[bottom] & (~std::uint64_t{0} << (from & 63U));
    while (word == 0U) {
      ++bottom;
      if (bottom == BOTTOM_WORDS) {
        return NONE;
      }
      word = bottom_[bottom];
    }
    return (bottom << 6U) + static_cast<std::size_t>(std::countr_zero(word));
  }

  // Highest occupied level at or below `from`. Same cost and same caveat as
  // next_set.
  [[nodiscard]] std::size_t prev_set(std::size_t from) const noexcept {
    if (Levels == 0) {
      return NONE;
    }

    std::size_t bottom = (from >= Levels ? Levels - 1U : from) >> 6U;
    const std::size_t start_bit = (from >= Levels ? 63U : (from & 63U));
    std::uint64_t word = bottom_[bottom];
    if (start_bit != 63U) {
      word &= (~std::uint64_t{0} >> (63U - start_bit));
    }

    while (word == 0U) {
      if (bottom == 0U) {
        return NONE;
      }
      --bottom;
      word = bottom_[bottom];
    }
    return (bottom << 6U) + highest_bit(word);
  }

  [[nodiscard]] std::size_t popcount() const noexcept {
    std::size_t total = 0;
    for (const std::uint64_t word : bottom_) {
      total += static_cast<std::size_t>(std::popcount(word));
    }
    return total;
  }

  void clear_all() noexcept {
    bottom_.fill(0U);
    middle_.fill(0U);
    top_ = 0U;
  }

  [[nodiscard]] static constexpr std::size_t memory_bytes() noexcept {
    return (BOTTOM_WORDS + MIDDLE_WORDS + 1U) * sizeof(std::uint64_t);
  }

 private:
  [[nodiscard]] static constexpr std::uint64_t bit(std::size_t index) noexcept {
    return std::uint64_t{1} << (index & 63U);
  }

  // bit_width returns one plus the index of the highest set bit, so subtracting
  // one gives the index. Undefined for zero, which every caller has excluded.
  [[nodiscard]] static constexpr std::size_t highest_bit(std::uint64_t word) noexcept {
    assert(word != 0U);
    return static_cast<std::size_t>(std::bit_width(word)) - 1U;
  }

  std::array<std::uint64_t, BOTTOM_WORDS> bottom_{};
  std::array<std::uint64_t, MIDDLE_WORDS> middle_{};
  std::uint64_t top_ = 0;
};

}  // namespace ob
