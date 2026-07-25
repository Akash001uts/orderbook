#pragma once

#include <cstdint>

#include "ob/types.hpp"

namespace ob {

// Participant identifier, used only by self trade prevention. Sixteen bits is
// ample for a simulator and it fits in padding that would otherwise be wasted.
using ParticipantId = std::uint16_t;
constexpr ParticipantId NO_PARTICIPANT = 0;

// A resting order living in the arena.
//
// Neighbours are 32-bit arena indices rather than pointers. Indices are half the
// width of a pointer on x86-64, which doubles the number of links that fit in a
// cache line and makes the whole arena relocatable: it can be memcpy'd or grown
// without fixing up a single link.
//
// Invariant: prev and next are either INVALID_INDEX or a live index strictly
// less than the arena capacity, and both refer to orders at the same price and
// on the same side. The level's head order has prev == INVALID_INDEX and the
// tail order has next == INVALID_INDEX.
//
// Layout note: the field order is chosen so that sizeof(Order) is exactly 40
// bytes with a single byte of tail padding, and so that the fields touched on
// cancel (prev, next, price, remaining) share one 16-byte span. The generation
// counter is present in every build, not just debug, so that release and debug
// layouts are identical. A layout that differed between the two would make the
// differential test, which runs in debug, prove nothing about the release build
// the benchmarks measure.
struct Order {
  OrderId id{};
  Timestamp timestamp{};
  ArenaIndex prev = INVALID_INDEX;
  ArenaIndex next = INVALID_INDEX;
  Ticks price{};
  std::uint32_t remaining = 0;
  std::uint32_t generation = 0;
  ParticipantId party = NO_PARTICIPANT;
  Side side = Side::buy;

  [[nodiscard]] constexpr Quantity remaining_qty() const noexcept { return Quantity{remaining}; }
};

static_assert(sizeof(Order) == 40, "Order layout regressed, see the layout note above");
static_assert(alignof(Order) == 8, "Order must stay 8-byte aligned for the arena");
static_assert(std::is_trivially_copyable_v<Order>, "arena relocation relies on trivial copy");

// Per-order share counts are 32 bits, matching the width of the ITCH 5.0 shares
// field. Aggregates are 64 bits. This is the boundary where a caller supplied
// Quantity narrows, so it is checked once here rather than at every use.
constexpr std::uint64_t MAX_ORDER_SHARES = 0xFFFFFFFFULL;

}  // namespace ob
