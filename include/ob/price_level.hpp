#pragma once

#include <cstdint>

#include "ob/types.hpp"

namespace ob {

// One price level: the head and tail of an intrusive FIFO of resting orders
// plus the two aggregates that queries need.
//
// Invariants:
//   order_count == 0  if and only if  head == tail == INVALID_INDEX
//   order_count == 0  implies  aggregate_qty == 0
//   aggregate_qty == sum of remaining over the orders reachable from head
//
// The level bitmap in book.hpp mirrors the order_count == 0 predicate. Any code
// path that takes a level from empty to occupied, or back, must update both or
// best_bid and best_ask will report a price with no liquidity behind it.
//
// Layout note: sizeof is 24 bytes, so 2.67 levels fit in a 64-byte cache line.
// Padding out to 32 would stop levels straddling line boundaries but costs 33
// percent more memory across a 65536 level band, which pushes the resident set
// past L2. The straddle is cheaper than the extra misses, so 24 stands. All
// four fields are read together when an aggressive order consumes a level, so
// there is no cold field to split out.
struct PriceLevel {
  Quantity aggregate_qty{};
  ArenaIndex head = INVALID_INDEX;
  ArenaIndex tail = INVALID_INDEX;
  std::uint32_t order_count = 0;

  [[nodiscard]] constexpr bool empty() const noexcept { return order_count == 0; }

  constexpr void reset() noexcept {
    aggregate_qty = Quantity{0};
    head = INVALID_INDEX;
    tail = INVALID_INDEX;
    order_count = 0;
  }
};

static_assert(sizeof(PriceLevel) == 24, "PriceLevel layout regressed, see the layout note above");
static_assert(alignof(PriceLevel) == 8, "PriceLevel alignment feeds the direct-index arithmetic");
static_assert(std::is_trivially_copyable_v<PriceLevel>, "band rebasing memcpy's levels");

}  // namespace ob
