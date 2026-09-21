#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "ob/book.hpp"
#include "ob/engine.hpp"
#include "ob/events.hpp"
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

}  // namespace
}  // namespace ob
