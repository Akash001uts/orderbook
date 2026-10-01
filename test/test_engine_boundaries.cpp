#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "ob/book.hpp"
#include "ob/engine.hpp"
#include "ob/events.hpp"
#include "ob/order_pool.hpp"
#include "ob/types.hpp"

// Public API boundary behaviour: construction rejects unusable configuration, the
// price-config validity helper agrees with it, events are delivered normally, and a
// sink too small to hold a command's events aborts rather than dropping one.
//
// These are asserted directly against the specification of the API, not against the
// reference book, because they are properties of the boundary rather than of the
// matching semantics the differential test already covers.

namespace ob {
namespace {

using TestEngine = Engine<CancelNewest, 4096>;

constexpr std::int64_t BASE_PRICE = 1000000;
constexpr std::int64_t TICK_SIZE = 100;

TestEngine::Config make_config() {
  TestEngine::Config config;
  config.price =
      PriceConfig{.tick_size = TICK_SIZE, .price_scale = 10000, .base_price = BASE_PRICE};
  config.arena_capacity = 1024;
  config.max_cold_levels_per_side = 32;
  config.initial_center = Ticks{0};
  return config;
}

Price price_at(std::int32_t tick) {
  return Price{BASE_PRICE + (static_cast<std::int64_t>(tick) * TICK_SIZE)};
}

Command add(std::uint64_t id,
            Side side,
            std::int32_t tick,
            std::uint64_t quantity,
            OrderType order_type = OrderType::limit) {
  Command command;
  command.type = CommandType::add;
  command.order_type = order_type;
  command.id = OrderId{id};
  command.side = side;
  command.price = price_at(tick);
  command.quantity = Quantity{quantity};
  command.timestamp = Timestamp{id};
  return command;
}

// ---------------------------------------------------------------------------
// PriceConfig validity helper
// ---------------------------------------------------------------------------

TEST(PriceConfigValidity, AcceptsAWellFormedConfig) {
  const PriceConfig config{.tick_size = 100, .price_scale = 10000, .base_price = 213000};
  EXPECT_TRUE(config.valid());
  EXPECT_EQ(config.validity_error(), nullptr);
}

TEST(PriceConfigValidity, RejectsNonPositiveTickSize) {
  EXPECT_FALSE((PriceConfig{.tick_size = 0, .price_scale = 10000, .base_price = 0}).valid());
  EXPECT_FALSE((PriceConfig{.tick_size = -100, .price_scale = 10000, .base_price = 0}).valid());
}

TEST(PriceConfigValidity, RejectsNonPositivePriceScale) {
  EXPECT_FALSE((PriceConfig{.tick_size = 100, .price_scale = 0, .base_price = 0}).valid());
  EXPECT_FALSE((PriceConfig{.tick_size = 100, .price_scale = -1, .base_price = 0}).valid());
}

TEST(PriceConfigValidity, RejectsBasePriceOffTheTickGrid) {
  const PriceConfig config{.tick_size = 100, .price_scale = 10000, .base_price = 150};
  EXPECT_FALSE(config.valid());
  ASSERT_NE(config.validity_error(), nullptr);
}

// ---------------------------------------------------------------------------
// Construction validation. The engine wraps the book, so constructing either with
// a bad configuration must throw before any storage is allocated.
// ---------------------------------------------------------------------------

TEST(EngineConstruction, AcceptsAValidConfig) {
  EXPECT_NO_THROW({ TestEngine engine(make_config()); });
  EXPECT_NO_THROW({ Book<4096> book(make_config()); });
}

TEST(EngineConstruction, RejectsZeroArenaCapacity) {
  TestEngine::Config config = make_config();
  config.arena_capacity = 0;
  EXPECT_THROW({ TestEngine engine(config); }, std::invalid_argument);
  EXPECT_THROW({ Book<4096> book(config); }, std::invalid_argument);
}

TEST(EngineConstruction, RejectsReservedArenaCapacity) {
  TestEngine::Config config = make_config();
  // INVALID_INDEX is the "no order" sentinel, so a capacity that reaches it would
  // make a valid slot indistinguishable from empty. This must throw rather than
  // attempt a multi-gigabyte allocation.
  config.arena_capacity = INVALID_INDEX;
  EXPECT_THROW({ TestEngine engine(config); }, std::invalid_argument);
}

TEST(EngineConstruction, RejectsNonPositiveTickSize) {
  TestEngine::Config config = make_config();
  config.price.tick_size = 0;
  EXPECT_THROW({ TestEngine engine(config); }, std::invalid_argument);
}

TEST(EngineConstruction, RejectsNonPositivePriceScale) {
  TestEngine::Config config = make_config();
  config.price.price_scale = 0;
  EXPECT_THROW({ TestEngine engine(config); }, std::invalid_argument);
}

TEST(EngineConstruction, RejectsMisalignedBasePrice) {
  TestEngine::Config config = make_config();
  config.price.base_price = BASE_PRICE + 1;  // not a multiple of tick_size
  EXPECT_THROW({ TestEngine engine(config); }, std::invalid_argument);
}

TEST(EngineConstruction, RejectsInitialCenterThatOverflowsTheBand) {
  TestEngine::Config config = make_config();
  // A centre at either int32 extreme pushes the band base past the Ticks range,
  // which would overflow centre_to_base and later rebases. Must throw.
  config.initial_center = Ticks{std::numeric_limits<std::int32_t>::min()};
  EXPECT_THROW({ TestEngine engine(config); }, std::invalid_argument);
  config.initial_center = Ticks{std::numeric_limits<std::int32_t>::max()};
  EXPECT_THROW({ TestEngine engine(config); }, std::invalid_argument);
}

TEST(EngineConstruction, AcceptsInitialCenterWithinTheBand) {
  TestEngine::Config config = make_config();
  config.initial_center = Ticks{0};
  EXPECT_NO_THROW({ TestEngine engine(config); });
}

// ---------------------------------------------------------------------------
// Direct OrderPool construction. The public constructor must reject an
// unusable capacity in debug and release alike, before the storage vector is
// allocated, because the former debug-only asserts vanished under NDEBUG.
// ---------------------------------------------------------------------------

TEST(OrderPoolConstruction, RejectsZeroCapacity) {
  EXPECT_THROW({ OrderPool pool(0); }, std::invalid_argument);
}

TEST(OrderPoolConstruction, RejectsReservedSentinelCapacityWithoutAllocating) {
  // A capacity of INVALID_INDEX would both alias the "no order" sentinel and
  // attempt a multi-gigabyte allocation. The throw must precede that allocation.
  EXPECT_THROW({ OrderPool pool(INVALID_INDEX); }, std::invalid_argument);
}

TEST(OrderPoolConstruction, AcceptsMinimalAndOrdinaryCapacity) {
  EXPECT_NO_THROW({ OrderPool pool(1); });
  EXPECT_NO_THROW({ OrderPool pool(1024); });
  OrderPool pool(1);
  EXPECT_EQ(pool.capacity(), 1U);
  const ArenaIndex index = pool.allocate();
  EXPECT_NE(index, INVALID_INDEX);
  EXPECT_TRUE(pool.full());
}

// ---------------------------------------------------------------------------
// Arithmetic representability. valid() bounds the configuration's
// structure, not its arithmetic range; representable()/tick_representable() answer
// the per-value question, and the command path checks them so no signed overflow
// is reached. These live in the normal test binary so the sanitizer presets
// exercise the boundary arithmetic.
// ---------------------------------------------------------------------------

TEST(PriceRepresentability, AcceptsPricesAtTheTickRangeLimits) {
  const PriceConfig config{.tick_size = TICK_SIZE, .price_scale = 10000, .base_price = BASE_PRICE};
  constexpr std::int64_t I32_MAX = std::numeric_limits<std::int32_t>::max();
  constexpr std::int64_t I32_MIN = std::numeric_limits<std::int32_t>::min();
  const Price high{BASE_PRICE + (I32_MAX * TICK_SIZE)};
  const Price low{BASE_PRICE + (I32_MIN * TICK_SIZE)};
  EXPECT_TRUE(config.representable(high));
  EXPECT_TRUE(config.representable(low));
  EXPECT_EQ(config.to_ticks(high).raw(), std::numeric_limits<std::int32_t>::max());
  EXPECT_EQ(config.to_ticks(low).raw(), std::numeric_limits<std::int32_t>::min());
}

TEST(PriceRepresentability, RejectsPricesJustBeyondTheTickRange) {
  const PriceConfig config{.tick_size = TICK_SIZE, .price_scale = 10000, .base_price = BASE_PRICE};
  constexpr std::int64_t I32_MAX = std::numeric_limits<std::int32_t>::max();
  constexpr std::int64_t I32_MIN = std::numeric_limits<std::int32_t>::min();
  const Price above{BASE_PRICE + (I32_MAX * TICK_SIZE) + TICK_SIZE};
  const Price below{BASE_PRICE + (I32_MIN * TICK_SIZE) - TICK_SIZE};
  EXPECT_FALSE(config.representable(above));
  EXPECT_FALSE(config.representable(below));
}

TEST(PriceRepresentability, RejectsExtremeRawPricesWithoutOverflow) {
  const PriceConfig config{.tick_size = TICK_SIZE, .price_scale = 10000, .base_price = BASE_PRICE};
  EXPECT_FALSE(config.representable(Price{std::numeric_limits<std::int64_t>::max()}));
  EXPECT_FALSE(config.representable(Price{std::numeric_limits<std::int64_t>::min()}));
}

TEST(PriceRepresentability, PrecomputedBoundsMatchTheTickLimits) {
  const PriceConfig config{.tick_size = TICK_SIZE, .price_scale = 10000, .base_price = BASE_PRICE};
  constexpr std::int64_t I32_MAX = std::numeric_limits<std::int32_t>::max();
  constexpr std::int64_t I32_MIN = std::numeric_limits<std::int32_t>::min();
  EXPECT_EQ(config.max_representable_raw(), BASE_PRICE + (I32_MAX * TICK_SIZE));
  EXPECT_EQ(config.min_representable_raw(), BASE_PRICE + (I32_MIN * TICK_SIZE));
  // The extreme prices are inside the bounds and representable; the engine derives
  // its per-command check from exactly these bounds.
  EXPECT_TRUE(config.representable(Price{config.max_representable_raw()}));
  EXPECT_TRUE(config.representable(Price{config.min_representable_raw()}));
}

TEST(PriceRepresentability, BoundsClampToTheInt64LimitWhenTheProductOverflows) {
  // A huge tick_size makes int32-extreme * tick_size overflow int64, so the bounds
  // fall back to the int64 subtraction limit rather than overflowing.
  const PriceConfig config{.tick_size = std::numeric_limits<std::int64_t>::max() / 2,
                           .price_scale = 10000,
                           .base_price = 0};
  ASSERT_TRUE(config.valid());
  EXPECT_EQ(config.max_representable_raw(), std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(config.min_representable_raw(), std::numeric_limits<std::int64_t>::min());
}

TEST(TickRepresentability, AcceptsInBandTicksAndRoundTrips) {
  const PriceConfig config{.tick_size = TICK_SIZE, .price_scale = 10000, .base_price = BASE_PRICE};
  EXPECT_TRUE(config.tick_representable(Ticks{0}));
  EXPECT_TRUE(config.tick_representable(Ticks{1000}));
  EXPECT_TRUE(config.tick_representable(Ticks{std::numeric_limits<std::int32_t>::max()}));
  EXPECT_TRUE(config.tick_representable(Ticks{std::numeric_limits<std::int32_t>::min()}));
  const Price round_trip = config.to_price(Ticks{1000});
  EXPECT_EQ(config.to_ticks(round_trip).raw(), 1000);
}

TEST(TickRepresentability, RejectsWhenTickTimesTickSizeWouldOverflow) {
  // A pathological tick_size so that a small tick already overflows int64.
  const PriceConfig config{.tick_size = std::numeric_limits<std::int64_t>::max() / 2,
                           .price_scale = 10000,
                           .base_price = 0};
  ASSERT_TRUE(config.valid());
  EXPECT_TRUE(config.tick_representable(Ticks{1}));
  EXPECT_FALSE(config.tick_representable(Ticks{3}));
}

TEST(EngineCommandValidation, RejectsNonRepresentablePriceAsBandOverflow) {
  TestEngine engine(make_config());
  EventRing<16> ring;
  Command command = add(1, Side::buy, 0, 10);
  command.price = Price{std::numeric_limits<std::int64_t>::max()};  // no valid tick
  engine.submit(command, ring);

  std::vector<ExecutionEvent> events;
  ExecutionEvent event;
  while (ring.pop(event)) {
    events.push_back(event);
  }
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::rejected);
  EXPECT_EQ(events[0].reason, RejectReason::band_overflow);
}

// ---------------------------------------------------------------------------
// Normal event delivery. A sink sized for the work receives every event.
// ---------------------------------------------------------------------------

TEST(EventDelivery, RestingOrderEmitsOneBookUpdate) {
  TestEngine engine(make_config());
  EventRing<1024> ring;
  engine.submit(add(1, Side::buy, 5, 10), ring);

  std::vector<ExecutionEvent> events;
  ExecutionEvent event;
  while (ring.pop(event)) {
    events.push_back(event);
  }
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::book_update);
  EXPECT_FALSE(ring.overflowed());
}

TEST(EventDelivery, MarketableOrderEmitsAFillPerLevelConsumed) {
  TestEngine engine(make_config());
  EventRing<1024> ring;
  for (std::uint64_t id = 1; id <= 3; ++id) {
    ring.clear();
    engine.submit(add(id, Side::sell, static_cast<std::int32_t>(4 + id), 10), ring);
  }

  ring.clear();
  engine.submit(add(4, Side::buy, 0, 30, OrderType::market), ring);

  std::vector<ExecutionEvent> events;
  ExecutionEvent event;
  while (ring.pop(event)) {
    events.push_back(event);
  }
  ASSERT_EQ(events.size(), 3U);
  for (const ExecutionEvent& fill : events) {
    EXPECT_EQ(fill.type, EventType::fill);
  }
  EXPECT_FALSE(ring.overflowed());
}

// ---------------------------------------------------------------------------
// Event-sink overflow. A command whose events do not fit the sink is a fatal
// sizing error: the engine aborts rather than dropping an event. Built entirely
// inside the death statement so it is robust to the threadsafe death-test style,
// which re-executes rather than forking.
// ---------------------------------------------------------------------------

TEST(EventOverflowDeathTest, AbortsWhenTheSinkCannotHoldACommandsEvents) {
  const auto overflow = [] {
    TestEngine engine(make_config());

    EventRing<1024> setup;
    for (std::uint64_t id = 1; id <= 3; ++id) {
      setup.clear();
      engine.submit(add(id, Side::sell, static_cast<std::int32_t>(4 + id), 10), setup);
    }

    // One slot cannot hold the three fills the market order produces, so the second
    // push is rejected and the engine aborts.
    EventRing<1> tiny;
    engine.submit(add(4, Side::buy, 0, 30, OrderType::market), tiny);
  };

  EXPECT_DEATH(overflow(), "event sink");
}

// ---------------------------------------------------------------------------
// Further sink and overflow scenarios: multiple orders at one price,
// multiple levels, accumulation across undrained commands, an already-full ring,
// self-trade and modify emissions, and the overflow flag on its own.
// ---------------------------------------------------------------------------

TEST(EventDelivery, MultipleRestingOrdersAtOnePriceEmitAFillEach) {
  TestEngine engine(make_config());
  EventRing<1024> ring;
  for (std::uint64_t id = 1; id <= 3; ++id) {  // three buyers at one tick
    ring.clear();
    engine.submit(add(id, Side::buy, 5, 10), ring);
  }

  ring.clear();
  engine.submit(add(4, Side::sell, 5, 30), ring);  // sweeps all three at that price

  std::vector<ExecutionEvent> events;
  ExecutionEvent event;
  while (ring.pop(event)) {
    events.push_back(event);
  }
  ASSERT_EQ(events.size(), 3U);
  for (const ExecutionEvent& fill : events) {
    EXPECT_EQ(fill.type, EventType::fill);
    EXPECT_EQ(fill.price.raw(), 5);
  }
  EXPECT_FALSE(ring.overflowed());
}

TEST(EventDelivery, AccumulatedUndrainedEventsFitASinkSizedForTheSum) {
  TestEngine engine(make_config());
  EventRing<8> ring;  // never cleared between commands
  for (std::uint64_t id = 1; id <= 4; ++id) {
    engine.submit(add(id, Side::buy, static_cast<std::int32_t>(id), 10), ring);
  }
  EXPECT_FALSE(ring.overflowed());

  int count = 0;
  ExecutionEvent event;
  while (ring.pop(event)) {
    ++count;
  }
  EXPECT_EQ(count, 4);  // one book update per resting add, all retained
}

TEST(EventDelivery, ModifyEmitsABookUpdate) {
  TestEngine engine(make_config());
  EventRing<1024> ring;
  engine.submit(add(1, Side::buy, 5, 10), ring);
  ring.clear();

  Command modify;
  modify.type = CommandType::modify;
  modify.id = OrderId{1};
  modify.quantity = Quantity{20};
  modify.timestamp = Timestamp{2};
  engine.submit(modify, ring);

  std::vector<ExecutionEvent> events;
  ExecutionEvent event;
  while (ring.pop(event)) {
    events.push_back(event);
  }
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::book_update);
  EXPECT_FALSE(ring.overflowed());
}

TEST(EventDelivery, SelfTradePreventionEmitsACancelForTheRestingOrder) {
  using OldestEngine = Engine<CancelOldest, 4096>;
  OldestEngine engine(make_config());
  EventRing<1024> ring;

  // A resting sell by party 7, then an aggressor buy by the same party that
  // crosses it. CancelOldest removes the resting order (self_trade) and lets the
  // aggressor continue, so a cancel is emitted before the remainder rests.
  Command resting = add(1, Side::sell, 5, 10);
  resting.party = 7;
  engine.submit(resting, ring);
  ring.clear();

  Command aggressor = add(2, Side::buy, 5, 10);
  aggressor.party = 7;
  engine.submit(aggressor, ring);

  std::vector<ExecutionEvent> events;
  ExecutionEvent event;
  while (ring.pop(event)) {
    events.push_back(event);
  }
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events[0].type, EventType::cancelled);
  EXPECT_EQ(events[0].reason, RejectReason::self_trade);
  EXPECT_FALSE(ring.overflowed());
}

TEST(EventRingOverflow, FlagTracksARejectedPushIndependently) {
  EventRing<1> ring;
  EXPECT_FALSE(ring.overflowed());
  EXPECT_TRUE(ring.push(ExecutionEvent{}));
  EXPECT_FALSE(ring.overflowed());
  EXPECT_FALSE(ring.push(ExecutionEvent{}));  // full
  EXPECT_TRUE(ring.overflowed());
}

TEST(EventOverflowDeathTest, AbortsWhenARingAlreadyFullRejectsASingleEvent) {
  const auto overflow = [] {
    TestEngine engine(make_config());
    EventRing<1> ring;
    engine.submit(add(1, Side::buy, 3, 10), ring);  // fills the only slot
    engine.submit(add(2, Side::buy, 4, 10), ring);  // one more event, ring full
  };
  EXPECT_DEATH(overflow(), "event sink");
}

TEST(EventOverflowDeathTest, AbortsOnAccumulatedUndrainedEvents) {
  const auto overflow = [] {
    TestEngine engine(make_config());
    EventRing<2> ring;  // holds two book updates; the third command overflows
    for (std::uint64_t id = 1; id <= 3; ++id) {
      engine.submit(add(id, Side::buy, static_cast<std::int32_t>(id), 10), ring);
    }
  };
  EXPECT_DEATH(overflow(), "event sink");
}

}  // namespace
}  // namespace ob
