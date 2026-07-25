#include <cstdint>
#include <type_traits>

#include <gtest/gtest.h>

#include "ob/events.hpp"
#include "ob/order.hpp"
#include "ob/price_level.hpp"
#include "ob/types.hpp"

namespace ob {
namespace {

// ---------------------------------------------------------------------------
// The strong typedefs must be mutually unconvertible. These are the assertions
// that make the type layer worth having: if any of them stops holding, a Price
// can reach a parameter expecting a Quantity and the compiler will stay silent.
// ---------------------------------------------------------------------------

static_assert(!std::is_same_v<Price, Ticks>);
static_assert(!std::is_same_v<Price, Quantity>);
static_assert(!std::is_same_v<Quantity, OrderId>);
static_assert(!std::is_same_v<OrderId, Timestamp>);
static_assert(!std::is_same_v<Timestamp, Sequence>);

static_assert(!std::is_convertible_v<Price, Ticks>);
static_assert(!std::is_convertible_v<Ticks, Price>);
static_assert(!std::is_convertible_v<Price, Quantity>);
static_assert(!std::is_convertible_v<Quantity, Price>);
static_assert(!std::is_convertible_v<Quantity, OrderId>);
static_assert(!std::is_convertible_v<OrderId, Quantity>);
static_assert(!std::is_convertible_v<Timestamp, Sequence>);
static_assert(!std::is_convertible_v<Sequence, Timestamp>);

// Neither direction of implicit conversion to the representation is allowed.
// Losing the outbound direction is what stops `int n = some_price;` compiling,
// and losing the inbound direction is what stops `Price p = 42;` compiling.
static_assert(!std::is_convertible_v<std::int64_t, Price>);
static_assert(!std::is_convertible_v<Price, std::int64_t>);
static_assert(!std::is_convertible_v<std::uint64_t, Quantity>);
static_assert(!std::is_convertible_v<Quantity, std::uint64_t>);
static_assert(!std::is_convertible_v<int, Ticks>);
static_assert(!std::is_convertible_v<Ticks, int>);

// Explicit construction is still available, and stays constexpr.
static_assert(std::is_constructible_v<Price, std::int64_t>);
static_assert(std::is_constructible_v<Quantity, std::uint64_t>);
static_assert(Price{500}.raw() == 500);
static_assert(Quantity{7}.raw() == 7);

// The wrapper costs nothing. If either of these fails the abstraction is no
// longer free and the whole approach needs revisiting.
static_assert(sizeof(Price) == sizeof(std::int64_t));
static_assert(sizeof(Ticks) == sizeof(std::int32_t));
static_assert(sizeof(Quantity) == sizeof(std::uint64_t));
static_assert(std::is_trivially_copyable_v<Price>);
static_assert(std::is_trivially_destructible_v<Price>);
static_assert(std::is_standard_layout_v<Price>);

// Same-type arithmetic is meaningful and permitted.
static_assert(Ticks{10} + Ticks{5} == Ticks{15});
static_assert(Ticks{10} - Ticks{15} == Ticks{-5});
static_assert(Quantity{100} - Quantity{40} == Quantity{60});
static_assert(Quantity{100} * 3 == Quantity{300});
static_assert(Ticks{3} < Ticks{4});
static_assert(Price{100} >= Price{100});

// Cross-type arithmetic must not compile. A positive static_assert cannot express
// that, so it is checked by detecting whether the expression is well formed.
template <typename A, typename B>
concept Addable = requires(A a, B b) { a + b; };

static_assert(Addable<Ticks, Ticks>);
static_assert(!Addable<Price, Quantity>);
static_assert(!Addable<Quantity, OrderId>);
static_assert(!Addable<Timestamp, Sequence>);

// ---------------------------------------------------------------------------
// Layout assertions duplicated here so a size regression fails the test suite
// with a readable name, not only the header that declares the struct.
// ---------------------------------------------------------------------------

TEST(Layout, HotStructSizes) {
  EXPECT_EQ(sizeof(Order), 40U);
  EXPECT_EQ(sizeof(PriceLevel), 24U);
  EXPECT_EQ(sizeof(Command), 40U);
  EXPECT_EQ(sizeof(ExecutionEvent), 56U);
  EXPECT_EQ(alignof(Order), 8U);
  EXPECT_EQ(alignof(PriceLevel), 8U);
}

TEST(Side, OppositeAndIndex) {
  EXPECT_EQ(opposite(Side::buy), Side::sell);
  EXPECT_EQ(opposite(Side::sell), Side::buy);
  EXPECT_EQ(side_index(Side::buy), 0U);
  EXPECT_EQ(side_index(Side::sell), 1U);
}

// ---------------------------------------------------------------------------
// Fixed point price conversion. Floating point never appears: a tick is an
// integer count and a price is an integer number of scaled units.
// ---------------------------------------------------------------------------

TEST(PriceConfig, RoundTripsThroughTickSpace) {
  // ITCH 5.0 prices carry four implied decimals, so 100.05 dollars is 1000500
  // scaled units, and a one cent tick is 100 scaled units.
  const PriceConfig config{.tick_size = 100, .price_scale = 10000, .base_price = 1000000};

  EXPECT_EQ(config.to_ticks(Price{1000000}), Ticks{0});
  EXPECT_EQ(config.to_ticks(Price{1000500}), Ticks{5});
  EXPECT_EQ(config.to_ticks(Price{999900}), Ticks{-1});

  EXPECT_EQ(config.to_price(Ticks{0}), Price{1000000});
  EXPECT_EQ(config.to_price(Ticks{5}), Price{1000500});
  EXPECT_EQ(config.to_price(Ticks{-1}), Price{999900});

  for (std::int32_t tick = -1000; tick <= 1000; ++tick) {
    const Ticks original{tick};
    EXPECT_EQ(config.to_ticks(config.to_price(original)), original);
  }
}

TEST(PriceConfig, DetectsOffTickPrices) {
  const PriceConfig config{.tick_size = 100, .price_scale = 10000, .base_price = 1000000};

  EXPECT_TRUE(config.on_tick_boundary(Price{1000500}));
  EXPECT_FALSE(config.on_tick_boundary(Price{1000501}));
  EXPECT_FALSE(config.on_tick_boundary(Price{1000550}));
}

// ---------------------------------------------------------------------------
// The event ring is the engine's only output channel, so its overflow behaviour
// is a correctness property rather than a convenience.
// ---------------------------------------------------------------------------

TEST(EventRing, PushPopFifoOrder) {
  EventRing<8> ring;
  ASSERT_TRUE(ring.empty());

  for (std::uint64_t i = 0; i < 8; ++i) {
    ExecutionEvent event;
    event.sequence = Sequence{i};
    EXPECT_TRUE(ring.push(event));
  }

  EXPECT_EQ(ring.size(), 8U);
  EXPECT_FALSE(ring.overflowed());

  for (std::uint64_t i = 0; i < 8; ++i) {
    ExecutionEvent out;
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out.sequence, Sequence{i});
  }

  EXPECT_TRUE(ring.empty());
}

TEST(EventRing, ReportsOverflowInsteadOfGrowing) {
  EventRing<4> ring;

  for (int i = 0; i < 4; ++i) {
    EXPECT_TRUE(ring.push(ExecutionEvent{}));
  }

  EXPECT_FALSE(ring.push(ExecutionEvent{}));
  EXPECT_TRUE(ring.overflowed());
  EXPECT_EQ(ring.size(), 4U);

  ring.clear();
  EXPECT_FALSE(ring.overflowed());
  EXPECT_TRUE(ring.empty());
}

TEST(EventRing, WrapsWithoutLosingEvents) {
  EventRing<4> ring;
  ExecutionEvent out;

  // Push and drain twelve events through a four slot ring. If the wrap used a
  // modulo on a non power of two, or reset the indices on drain, this would
  // either mis-order or drop.
  for (std::uint64_t i = 0; i < 12; ++i) {
    ExecutionEvent event;
    event.sequence = Sequence{i};
    ASSERT_TRUE(ring.push(event));
    ASSERT_TRUE(ring.pop(out));
    EXPECT_EQ(out.sequence, Sequence{i});
  }

  EXPECT_FALSE(ring.overflowed());
}

}  // namespace
}  // namespace ob
