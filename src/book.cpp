#include "ob/book.hpp"

#include <array>
#include <map>
#include <memory>
#include <utility>

namespace ob {

// The one std::map in the project, quarantined in a translation unit so that no
// header on the hot path can reach it. Every operation here allocates, chases
// pointers, and costs O(log n), which is exactly why prices that land here are
// prices the band does not cover.
struct ColdLevels::Impl {
  explicit Impl(std::size_t max_levels_per_side) : max_levels(max_levels_per_side) {}

  std::array<std::map<Ticks, PriceLevel>, 2> sides{};
  std::size_t max_levels;
};

ColdLevels::ColdLevels(std::size_t max_levels_per_side)
    : impl_(std::make_unique<Impl>(max_levels_per_side)) {}

ColdLevels::~ColdLevels() = default;

PriceLevel* ColdLevels::level_for(Side side, Ticks price) {
  std::map<Ticks, PriceLevel>& levels = impl_->sides[side_index(side)];

  const auto existing = levels.find(price);
  if (existing != levels.end()) {
    return &existing->second;
  }

  // The cap is the only thing standing between an adversarial price distribution
  // and unbounded memory growth, so it is enforced before the insert rather than
  // checked afterwards.
  if (levels.size() >= impl_->max_levels) {
    return nullptr;
  }

  return &levels.emplace(price, PriceLevel{}).first->second;
}

PriceLevel* ColdLevels::find(Side side, Ticks price) noexcept {
  std::map<Ticks, PriceLevel>& levels = impl_->sides[side_index(side)];
  const auto found = levels.find(price);
  return found == levels.end() ? nullptr : &found->second;
}

const PriceLevel* ColdLevels::find(Side side, Ticks price) const noexcept {
  const std::map<Ticks, PriceLevel>& levels = impl_->sides[side_index(side)];
  const auto found = levels.find(price);
  return found == levels.end() ? nullptr : &found->second;
}

void ColdLevels::erase(Side side, Ticks price) noexcept {
  impl_->sides[side_index(side)].erase(price);
}

bool ColdLevels::empty(Side side) const noexcept {
  return impl_->sides[side_index(side)].empty();
}

std::size_t ColdLevels::size(Side side) const noexcept {
  return impl_->sides[side_index(side)].size();
}

std::size_t ColdLevels::count_in_range(Side side, Ticks low, Ticks high) const noexcept {
  const std::map<Ticks, PriceLevel>& levels = impl_->sides[side_index(side)];

  std::size_t count = 0;
  for (auto cursor = levels.lower_bound(low); cursor != levels.end() && cursor->first <= high;
       ++cursor) {
    ++count;
  }
  return count;
}

bool ColdLevels::best(Side side, Ticks& out) const noexcept {
  const std::map<Ticks, PriceLevel>& levels = impl_->sides[side_index(side)];
  if (levels.empty()) {
    return false;
  }

  // Best means highest for a buyer and lowest for a seller. The map is ordered
  // ascending, so the buy side reads from the back.
  out = side == Side::buy ? levels.rbegin()->first : levels.begin()->first;
  return true;
}

void ColdLevels::extract_range(Side side,
                               Ticks low,
                               Ticks high,
                               std::vector<std::pair<Ticks, PriceLevel>>& out) {
  std::map<Ticks, PriceLevel>& levels = impl_->sides[side_index(side)];

  auto cursor = levels.lower_bound(low);
  while (cursor != levels.end() && cursor->first <= high) {
    out.emplace_back(cursor->first, cursor->second);
    cursor = levels.erase(cursor);
  }
}

}  // namespace ob
