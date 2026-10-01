#pragma once

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "ob/order.hpp"
#include "ob/types.hpp"

namespace ob {

// Pre-allocated arena of orders with an intrusive free list.
//
// Every byte of storage is acquired once, in the constructor. allocate and
// deallocate touch the free list head and exactly one Order, so both are O(1)
// and neither reaches the allocator. That is what makes the zero-allocation
// assertion in test_book.cpp checkable rather than aspirational.
//
// The free list is intrusive: a free slot stores the index of the next free slot
// in its own `next` field, which is meaningless while the slot is free anyway. A
// separate free list array would cost another 4 bytes per slot and a second cache
// line touch on every allocate.
//
// Liveness is encoded in the parity of `generation`: odd means live, even means
// free. allocate and deallocate each increment it. This means a stale index is
// detectable from the index alone, with no need to widen links into 64-bit
// {index, generation} handles. Keeping links at 32 bits is the entire reason the
// arena exists, so paying for use-after-free detection in bits per slot rather
// than bits per link is the right way round.
class OrderPool {
 public:
  // checked_capacity runs inside the member initializer list, before slots_ is
  // constructed, so an invalid request throws rather than reaching an invalid free
  // list (zero capacity has no slot 0 to seed the head) or attempting the
  // reserved-sentinel-sized allocation. The former debug-only assertions vanished
  // under NDEBUG, leaving this public constructor unguarded in release.
  explicit OrderPool(std::uint32_t capacity) : slots_(checked_capacity(capacity)) {
    rebuild_free_list();
  }

  // Returns INVALID_INDEX when the arena is full. The caller must check: there
  // is no exception to throw and no allocator to fall back to, by design.
  [[nodiscard]] ArenaIndex allocate() noexcept {
    const ArenaIndex index = free_head_;
    if (index == INVALID_INDEX) {
      return INVALID_INDEX;
    }

    Order& slot = slots_[index];
    free_head_ = slot.next;
    slot.prev = INVALID_INDEX;
    slot.next = INVALID_INDEX;
    ++slot.generation;
    ++live_count_;

    assert((slot.generation & 1U) == 1U && "generation parity must mark a live slot odd");
    return index;
  }

  // The caller unlinks the order from its price level first. This only returns
  // the slot to the free list.
  void deallocate(ArenaIndex index) noexcept {
    assert(is_live(index));

    Order& slot = slots_[index];
    ++slot.generation;
    slot.prev = INVALID_INDEX;
    slot.next = free_head_;
    free_head_ = index;
    --live_count_;

    assert((slot.generation & 1U) == 0U && "generation parity must mark a free slot even");
  }

  [[nodiscard]] Order& operator[](ArenaIndex index) noexcept {
    assert(is_live(index));
    return slots_[index];
  }

  [[nodiscard]] const Order& operator[](ArenaIndex index) const noexcept {
    assert(is_live(index));
    return slots_[index];
  }

  // The use-after-free check. Cheap enough to assert on in debug on every
  // dereference, which is where it earns its keep.
  [[nodiscard]] bool is_live(ArenaIndex index) const noexcept {
    return index < capacity() && (slots_[index].generation & 1U) == 1U;
  }

  [[nodiscard]] std::uint32_t capacity() const noexcept {
    return static_cast<std::uint32_t>(slots_.size());
  }

  [[nodiscard]] std::uint32_t live_count() const noexcept { return live_count_; }

  [[nodiscard]] bool full() const noexcept { return free_head_ == INVALID_INDEX; }

  [[nodiscard]] std::size_t memory_bytes() const noexcept { return slots_.size() * sizeof(Order); }

  // Returns every slot to the free list. Generations are preserved, not reset,
  // so an index captured before a clear still fails is_live if its slot has not
  // been handed out again.
  void clear() noexcept {
    for (Order& slot : slots_) {
      if ((slot.generation & 1U) == 1U) {
        ++slot.generation;
      }
    }
    rebuild_free_list();
  }

 private:
  // Reject capacities that cannot back a valid arena, in debug and release alike,
  // before any storage is allocated. Book validates the same bounds first through
  // validate_config (with its own "Book:" message the engine tests assert), so this
  // only fires on direct OrderPool construction. Both checks reference the shared
  // INVALID_INDEX sentinel so the accepted range cannot drift between them.
  [[nodiscard]] static std::uint32_t checked_capacity(std::uint32_t capacity) {
    if (capacity == 0U) {
      throw std::invalid_argument("OrderPool: capacity must be greater than zero");
    }
    if (capacity >= INVALID_INDEX) {
      throw std::invalid_argument(
          "OrderPool: capacity must be below the reserved INVALID_INDEX sentinel");
    }
    return capacity;
  }

  // Walking the whole arena here is deliberate: it is the warmup that first
  // touches every page, so no later allocate pays a page fault. A benchmark that
  // measured that fault would be measuring the kernel, not the book.
  void rebuild_free_list() noexcept {
    const std::uint32_t count = capacity();
    for (std::uint32_t i = 0; i + 1 < count; ++i) {
      slots_[i].next = i + 1;
    }
    slots_[count - 1].next = INVALID_INDEX;
    free_head_ = 0;
    live_count_ = 0;
  }

  std::vector<Order> slots_;
  ArenaIndex free_head_ = INVALID_INDEX;
  std::uint32_t live_count_ = 0;
};

}  // namespace ob
