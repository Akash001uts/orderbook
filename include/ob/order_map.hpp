#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <vector>

#include "ob/types.hpp"

namespace ob {

// Open addressing map from OrderId to arena index, used by cancel and modify.
//
// Linear probing over a power of two capacity, Fibonacci hashing, backward shift
// deletion, keys and values in separate arrays. Chosen over `std::unordered_map`
// because a bucket-of-nodes container puts a pointer chase and an allocation on
// the first step of every cancel. The full reasoning, and the alternatives
// weighed against it, are in DESIGN.md, "Open addressing over
// std::unordered_map".
//
// Invariants this file must preserve:
//
//   A key of zero marks an empty slot. OrderId zero is reserved and rejected on
//   insert, which is what removes the need for a separate occupancy array or a
//   tombstone byte. ITCH 5.0 order reference numbers start at one, so no real
//   venue collides with it.
//
//   Deletion is by backward shift and never by tombstone. A tombstone counts
//   toward probe length forever, so a book that cancels heavily degrades
//   permanently.
//
//   An entry may move back into the hole only if its ideal slot does not lie
//   cyclically within the range being closed. `EveryKeyStaysReachableAcrossHeavyChurn`
//   in test_book.cpp exists because a naive form of that condition passes every
//   simple test and loses keys under churn.
class OrderIdMap {
 public:
  // Capacity is rounded up to a power of two at or above twice expected_orders,
  // giving a maximum load factor of 0.5. The knee in linear probing's expected
  // probe count is sharp just above that, and the cost of being on the wrong side
  // of it is paid on every cancel. Figures in DESIGN.md.
  explicit OrderIdMap(std::uint32_t expected_orders) {
    const std::uint64_t wanted = static_cast<std::uint64_t>(expected_orders) * 2U;
    std::uint64_t capacity = 64;
    while (capacity < wanted) {
      capacity <<= 1U;
    }

    keys_.assign(static_cast<std::size_t>(capacity), 0U);
    values_.assign(static_cast<std::size_t>(capacity), INVALID_INDEX);
    mask_ = static_cast<std::size_t>(capacity) - 1U;
    shift_ = static_cast<unsigned>(64 - std::countr_zero(capacity));
  }

  // False on a duplicate key, on the reserved zero key, or when the table is at
  // its load limit. The caller decides what that means; the map does not grow,
  // because growing means allocating.
  [[nodiscard]] bool insert(OrderId id, ArenaIndex index) noexcept {
    if (id.raw() == 0U || size_ > mask_ / 2U) {
      return false;
    }

    std::size_t slot = slot_for(id);
    while (keys_[slot] != 0U) {
      if (keys_[slot] == id.raw()) {
        return false;
      }
      slot = (slot + 1U) & mask_;
    }

    keys_[slot] = id.raw();
    values_[slot] = index;
    ++size_;
    return true;
  }

  [[nodiscard]] ArenaIndex find(OrderId id) const noexcept {
    if (id.raw() == 0U) {
      return INVALID_INDEX;
    }

    std::size_t slot = slot_for(id);
    while (keys_[slot] != 0U) {
      if (keys_[slot] == id.raw()) {
        return values_[slot];
      }
      slot = (slot + 1U) & mask_;
    }
    return INVALID_INDEX;
  }

  [[nodiscard]] bool erase(OrderId id) noexcept {
    if (id.raw() == 0U) {
      return false;
    }

    std::size_t hole = slot_for(id);
    while (keys_[hole] != 0U && keys_[hole] != id.raw()) {
      hole = (hole + 1U) & mask_;
    }
    if (keys_[hole] == 0U) {
      return false;
    }

    // Opening the hole before the scan matters for termination as well as for
    // clarity: it guarantees the scan below meets an empty slot even in the
    // pathological case where it wraps all the way round.
    keys_[hole] = 0U;
    values_[hole] = INVALID_INDEX;
    --size_;

    std::size_t probe = hole;
    while (true) {
      probe = (probe + 1U) & mask_;
      if (keys_[probe] == 0U) {
        break;
      }

      // An entry may only move back into the hole if its ideal slot does not lie
      // cyclically within (hole, probe]. If it did, moving it back past its own
      // ideal slot would put it before the point where a lookup starts probing,
      // and the key would become unreachable. This is the whole subtlety of
      // backward shift deletion and it is why tombstones are the common choice.
      const std::size_t ideal = slot_for(OrderId{keys_[probe]});
      const bool ideal_inside =
          (hole <= probe) ? (hole < ideal && ideal <= probe) : (hole < ideal || ideal <= probe);
      if (ideal_inside) {
        continue;
      }

      keys_[hole] = keys_[probe];
      values_[hole] = values_[probe];
      keys_[probe] = 0U;
      values_[probe] = INVALID_INDEX;
      hole = probe;
    }

    return true;
  }

  [[nodiscard]] std::uint32_t size() const noexcept { return size_; }

  [[nodiscard]] std::size_t capacity() const noexcept { return mask_ + 1U; }

  [[nodiscard]] double load_factor() const noexcept {
    return static_cast<double>(size_) / static_cast<double>(capacity());
  }

  [[nodiscard]] std::size_t memory_bytes() const noexcept {
    return (keys_.size() * sizeof(std::uint64_t)) + (values_.size() * sizeof(ArenaIndex));
  }

  void clear() noexcept {
    keys_.assign(keys_.size(), 0U);
    values_.assign(values_.size(), INVALID_INDEX);
    size_ = 0;
  }

  // Total probes to locate every live key, divided by the number of keys. Used by
  // the benchmark report rather than by the book, so that the published numbers
  // come with the load factor they were measured at.
  [[nodiscard]] double mean_probe_count() const noexcept {
    if (size_ == 0) {
      return 0.0;
    }

    std::uint64_t probes = 0;
    for (std::size_t slot = 0; slot < keys_.size(); ++slot) {
      if (keys_[slot] == 0U) {
        continue;
      }
      const std::size_t ideal = slot_for(OrderId{keys_[slot]});
      probes += static_cast<std::uint64_t>((slot - ideal) & mask_) + 1U;
    }
    return static_cast<double>(probes) / static_cast<double>(size_);
  }

 private:
  // Fibonacci hashing: one multiply by the 64-bit golden ratio, then take the
  // high bits of the product. Identity-masking was the alternative and it is
  // tempting, because ITCH order reference numbers arrive nearly sequential and
  // identity-masking would place them with no collisions at all. It fails when
  // the venue's numbering has a stride sharing a factor with the capacity, which
  // is a property of the data feed rather than of this code, and the failure mode
  // is silent clustering. The multiply costs three cycles and removes the
  // dependency on someone else's numbering scheme.
  [[nodiscard]] std::size_t slot_for(OrderId id) const noexcept {
    static constexpr std::uint64_t GOLDEN_RATIO_64 = 0x9E3779B97F4A7C15ULL;
    return static_cast<std::size_t>((id.raw() * GOLDEN_RATIO_64) >> shift_);
  }

  std::vector<std::uint64_t> keys_;
  std::vector<ArenaIndex> values_;
  std::size_t mask_ = 0;
  unsigned shift_ = 0;
  std::uint32_t size_ = 0;
};

}  // namespace ob
