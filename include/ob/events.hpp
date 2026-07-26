#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "ob/order.hpp"
#include "ob/types.hpp"

namespace ob {

enum class CommandType : std::uint8_t { add, cancel, modify };

enum class OrderType : std::uint8_t {
  limit,
  market,
  immediate_or_cancel,
  fill_or_kill,
  post_only,
};

// One unit of input. Every field the engine needs arrives here, including the
// timestamp: the engine reads no clock, so replaying the same command sequence
// twice produces byte-identical state. That determinism is what makes the
// differential test in test/test_differential.cpp meaningful.
//
// price is a Price, not a Ticks, because this is the API boundary. The engine
// converts once on entry via PriceConfig and works in tick space thereafter.
struct Command {
  OrderId id{};
  Price price{};
  Quantity quantity{};
  Timestamp timestamp{};
  ParticipantId party = NO_PARTICIPANT;
  CommandType type = CommandType::add;
  OrderType order_type = OrderType::limit;
  Side side = Side::buy;
};

static_assert(sizeof(Command) == 40, "Command layout regressed");

enum class EventType : std::uint8_t {
  fill,          // resting order fully consumed by an aggressor
  partial_fill,  // resting order reduced, remainder still on the book
  cancelled,     // order removed by an explicit cancel or by IOC expiry
  rejected,      // command refused, no state was mutated
  book_update,   // aggregate at a price level changed
};

enum class RejectReason : std::uint8_t {
  none,
  unknown_order,           // cancel or modify naming an id the book does not hold
  duplicate_order,         // add reusing a live id
  zero_quantity,           // add or modify with no shares
  quantity_too_large,      // more shares than the ITCH 5.0 field width expresses
  off_tick,                // price is not an exact multiple of tick_size
  band_overflow,           // price outside the band and the cold path is disabled
  arena_exhausted,         // no free arena slot, the book is at capacity
  insufficient_liquidity,  // fill-or-kill could not be filled in full
  would_cross,             // post-only order would have taken liquidity
  self_trade,              // blocked by the self trade prevention policy
};

// One unit of output. The engine appends these and does nothing else: it never
// writes to stdout, never logs, never allocates. What the events mean is the
// consumer's problem.
//
// For a fill, maker_id is the resting order and taker_id is the aggressor. For a
// cancel or reject only the affected id is set and the other is left at zero.
// remaining is the maker's residual after the fill, which lets a consumer track
// queue depletion without holding its own copy of the book.
struct ExecutionEvent {
  OrderId taker_id{};
  OrderId maker_id{};
  Quantity quantity{};
  Quantity remaining{};
  Timestamp timestamp{};
  Sequence sequence{};
  Ticks price{};
  EventType type = EventType::book_update;
  Side aggressor_side = Side::buy;
  RejectReason reason = RejectReason::none;
};

static_assert(sizeof(ExecutionEvent) == 56, "ExecutionEvent layout regressed");
static_assert(std::is_trivially_copyable_v<ExecutionEvent>, "events are memcpy'd into the ring");

// Fixed capacity event sink owned by the caller, not by the engine.
//
// Indices are plain integers, not atomics: the engine and its consumer run on
// the same thread, so there is no cross-thread publication to order. Making them
// atomic would add a lock xadd to the hot path to buy a guarantee nothing needs.
//
// Capacity must be a power of two so the wrap is a mask rather than a modulo.
// On overflow push returns false and sets the overflow flag; it never grows,
// because growing means allocating. Callers drain between commands and tests
// assert overflowed() stays false, since a dropped event would desynchronise the
// differential comparison.
template <std::size_t Capacity>
class EventRing {
  static_assert(Capacity > 0 && (Capacity & (Capacity - 1)) == 0,
                "EventRing capacity must be a power of two");

 public:
  static constexpr std::size_t CAPACITY = Capacity;

  [[nodiscard]] bool push(const ExecutionEvent& event) noexcept {
    if (size() == Capacity) {
      overflowed_ = true;
      return false;
    }
    storage_[write_ & MASK] = event;
    ++write_;
    return true;
  }

  [[nodiscard]] bool pop(ExecutionEvent& out) noexcept {
    if (read_ == write_) {
      return false;
    }
    out = storage_[read_ & MASK];
    ++read_;
    return true;
  }

  [[nodiscard]] std::size_t size() const noexcept { return write_ - read_; }

  [[nodiscard]] bool empty() const noexcept { return read_ == write_; }

  [[nodiscard]] bool overflowed() const noexcept { return overflowed_; }

  void clear() noexcept {
    read_ = 0;
    write_ = 0;
    overflowed_ = false;
  }

 private:
  static constexpr std::size_t MASK = Capacity - 1;

  std::array<ExecutionEvent, Capacity> storage_{};
  std::size_t read_ = 0;
  std::size_t write_ = 0;
  bool overflowed_ = false;
};

}  // namespace ob
