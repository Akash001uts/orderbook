#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "ob/engine.hpp"
#include "ob/events.hpp"

namespace ob {
namespace {

// Matching semantics, asserted directly against the specification rather than
// against the reference book.
//
// This file exists because of a real limitation of differential testing: two
// implementations that share a misconception agree with each other perfectly. The
// differential test proves the fast book and the naive book compute the same
// thing; these tests are what pin down that the thing they compute is the thing
// the specification asks for.

using TestEngine = Engine<CancelNewest, 4096>;
using Ring = EventRing<1024>;

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
            OrderType order_type = OrderType::limit,
            ParticipantId party = NO_PARTICIPANT) {
  Command command;
  command.type = CommandType::add;
  command.order_type = order_type;
  command.id = OrderId{id};
  command.side = side;
  command.price = price_at(tick);
  command.quantity = Quantity{quantity};
  command.timestamp = Timestamp{id};
  command.party = party;
  return command;
}

Command cancel(std::uint64_t id) {
  Command command;
  command.type = CommandType::cancel;
  command.id = OrderId{id};
  command.timestamp = Timestamp{id};
  return command;
}

class EngineFixture : public ::testing::Test {
 protected:
  std::vector<ExecutionEvent> submit(const Command& command) {
    ring_.clear();
    engine_.submit(command, ring_);

    std::vector<ExecutionEvent> events;
    ExecutionEvent event;
    while (ring_.pop(event)) {
      events.push_back(event);
    }
    EXPECT_FALSE(ring_.overflowed());
    return events;
  }

  [[nodiscard]] std::vector<std::uint64_t> queue_at(Side side, std::int32_t tick) const {
    std::vector<std::uint64_t> ids;
    for (ArenaIndex index = engine_.book().first_order_at(side, Ticks{tick});
         index != INVALID_INDEX;
         index = engine_.book().pool()[index].next) {
      ids.push_back(engine_.book().pool()[index].id.raw());
    }
    return ids;
  }

  // Protected rather than private because a GoogleTest fixture exposes its state to
  // the test bodies that derive from it. That is the framework's design, not a
  // leaked invariant.
  // NOLINTBEGIN(cppcoreguidelines-non-private-member-variables-in-classes)
  TestEngine engine_{make_config()};
  Ring ring_;
  // NOLINTEND(cppcoreguidelines-non-private-member-variables-in-classes)
};

// ---------------------------------------------------------------------------
// Price-time priority
// ---------------------------------------------------------------------------

TEST_F(EngineFixture, AggressorConsumesBetterPricesFirst) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);
  ASSERT_EQ(submit(add(2, Side::sell, 3, 10)).size(), 1U);
  ASSERT_EQ(submit(add(3, Side::sell, 4, 10)).size(), 1U);

  // A buyer paying up to 5 must take 3, then 4, then 5. Price priority outranks
  // arrival order across levels.
  const std::vector<ExecutionEvent> events = submit(add(4, Side::buy, 5, 30));

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].price, Ticks{3});
  EXPECT_EQ(events[0].maker_id, OrderId{2});
  EXPECT_EQ(events[1].price, Ticks{4});
  EXPECT_EQ(events[1].maker_id, OrderId{3});
  EXPECT_EQ(events[2].price, Ticks{5});
  EXPECT_EQ(events[2].maker_id, OrderId{1});

  for (const ExecutionEvent& event : events) {
    EXPECT_EQ(event.type, EventType::fill);
    EXPECT_EQ(event.quantity, Quantity{10});
    EXPECT_EQ(event.aggressor_side, Side::buy);
  }
}

TEST_F(EngineFixture, WithinALevelArrivalOrderWins) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);
  ASSERT_EQ(submit(add(2, Side::sell, 5, 10)).size(), 1U);
  ASSERT_EQ(submit(add(3, Side::sell, 5, 10)).size(), 1U);

  const std::vector<ExecutionEvent> events = submit(add(4, Side::buy, 5, 25));

  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].maker_id, OrderId{1});
  EXPECT_EQ(events[1].maker_id, OrderId{2});
  EXPECT_EQ(events[2].maker_id, OrderId{3});

  // The last maker is only partly consumed and keeps the rest of its place.
  EXPECT_EQ(events[2].type, EventType::partial_fill);
  EXPECT_EQ(events[2].quantity, Quantity{5});
  EXPECT_EQ(events[2].remaining, Quantity{5});
  EXPECT_EQ(queue_at(Side::sell, 5), (std::vector<std::uint64_t>{3}));
}

TEST_F(EngineFixture, AnUnfilledRemainderRestsAndBecomesTheBest) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);

  const std::vector<ExecutionEvent> events = submit(add(2, Side::buy, 5, 25));

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].type, EventType::fill);
  EXPECT_EQ(events[1].type, EventType::book_update);
  EXPECT_EQ(events[1].quantity, Quantity{15});

  EXPECT_EQ(engine_.book().best_bid(), Ticks{5});
  EXPECT_FALSE(engine_.book().best_ask().has_value());
}

TEST_F(EngineFixture, ANonCrossingLimitDoesNotTrade) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);

  const std::vector<ExecutionEvent> events = submit(add(2, Side::buy, 4, 10));

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::book_update);
  EXPECT_EQ(engine_.book().best_bid(), Ticks{4});
  EXPECT_EQ(engine_.book().best_ask(), Ticks{5});
}

// ---------------------------------------------------------------------------
// Order types
// ---------------------------------------------------------------------------

TEST_F(EngineFixture, MarketOrderIgnoresPriceAndCancelsItsRemainder) {
  ASSERT_EQ(submit(add(1, Side::sell, 50, 10)).size(), 1U);

  // The command's price field is far below the resting ask. A market order must
  // ignore it entirely.
  const std::vector<ExecutionEvent> events = submit(add(2, Side::buy, -50, 25, OrderType::market));

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].type, EventType::fill);
  EXPECT_EQ(events[0].price, Ticks{50});
  EXPECT_EQ(events[0].quantity, Quantity{10});

  EXPECT_EQ(events[1].type, EventType::cancelled);
  EXPECT_EQ(events[1].quantity, Quantity{15});
  // A market order has no price, so the event reports zero rather than leaking
  // the synthetic limit the matcher uses internally.
  EXPECT_EQ(events[1].price, Ticks{0});

  EXPECT_FALSE(engine_.book().best_bid().has_value());
}

TEST_F(EngineFixture, ImmediateOrCancelKeepsWhatItTookAndDropsTheRest) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);

  const std::vector<ExecutionEvent> events =
      submit(add(2, Side::buy, 5, 25, OrderType::immediate_or_cancel));

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].type, EventType::fill);
  EXPECT_EQ(events[0].quantity, Quantity{10});
  EXPECT_EQ(events[1].type, EventType::cancelled);
  EXPECT_EQ(events[1].quantity, Quantity{15});

  EXPECT_FALSE(engine_.book().best_bid().has_value());
}

TEST_F(EngineFixture, FillOrKillRejectsWithoutMutatingAnything) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);
  ASSERT_EQ(submit(add(2, Side::sell, 6, 10)).size(), 1U);

  // Twenty five wanted against twenty available at acceptable prices.
  const std::vector<ExecutionEvent> events =
      submit(add(3, Side::buy, 6, 25, OrderType::fill_or_kill));

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::rejected);
  EXPECT_EQ(events[0].reason, RejectReason::insufficient_liquidity);

  // The critical assertion: no partial mutation on rejection. Both resting orders
  // must be untouched, not merely present.
  EXPECT_EQ(engine_.book().total_qty_at(Side::sell, Ticks{5}), Quantity{10});
  EXPECT_EQ(engine_.book().total_qty_at(Side::sell, Ticks{6}), Quantity{10});
  EXPECT_EQ(engine_.book().pool().live_count(), 2U);
}

TEST_F(EngineFixture, FillOrKillFillsCompletelyWhenLiquidityIsThere) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);
  ASSERT_EQ(submit(add(2, Side::sell, 6, 15)).size(), 1U);

  const std::vector<ExecutionEvent> events =
      submit(add(3, Side::buy, 6, 25, OrderType::fill_or_kill));

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].type, EventType::fill);
  EXPECT_EQ(events[1].type, EventType::fill);
  EXPECT_EQ(engine_.book().pool().live_count(), 0U);
}

TEST_F(EngineFixture, FillOrKillIgnoresLiquidityBeyondItsLimit) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);
  ASSERT_EQ(submit(add(2, Side::sell, 9, 100)).size(), 1U);

  // Plenty of size exists, but not at a price this order will pay.
  const std::vector<ExecutionEvent> events =
      submit(add(3, Side::buy, 5, 25, OrderType::fill_or_kill));

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].reason, RejectReason::insufficient_liquidity);
  EXPECT_EQ(engine_.book().pool().live_count(), 2U);
}

TEST_F(EngineFixture, PostOnlyRejectsRatherThanRepricing) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);

  const std::vector<ExecutionEvent> events = submit(add(2, Side::buy, 5, 10, OrderType::post_only));

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::rejected);
  EXPECT_EQ(events[0].reason, RejectReason::would_cross);

  // Silently repricing to 4 would be the tempting alternative and it is wrong: a
  // participant who asked to be a maker did not consent to a different price.
  EXPECT_FALSE(engine_.book().best_bid().has_value());
  EXPECT_EQ(engine_.book().pool().live_count(), 1U);
}

TEST_F(EngineFixture, PostOnlyRestsWhenItDoesNotCross) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);

  const std::vector<ExecutionEvent> events = submit(add(2, Side::buy, 4, 10, OrderType::post_only));

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::book_update);
  EXPECT_EQ(engine_.book().best_bid(), Ticks{4});
}

// ---------------------------------------------------------------------------
// Self trade prevention
// ---------------------------------------------------------------------------

TEST_F(EngineFixture, CancelNewestStopsTheAggressorAndKeepsTheRestingOrder) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10, OrderType::limit, 7)).size(), 1U);

  const std::vector<ExecutionEvent> events = submit(add(2, Side::buy, 5, 10, OrderType::limit, 7));

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::cancelled);
  EXPECT_EQ(events[0].reason, RejectReason::self_trade);
  EXPECT_EQ(events[0].taker_id, OrderId{2});

  // The incoming order is the newer of the two, so it is the one that gives way.
  EXPECT_EQ(engine_.book().total_qty_at(Side::sell, Ticks{5}), Quantity{10});
  EXPECT_EQ(engine_.book().pool().live_count(), 1U);
}

TEST_F(EngineFixture, DifferentParticipantsTradeNormally) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10, OrderType::limit, 7)).size(), 1U);

  const std::vector<ExecutionEvent> events = submit(add(2, Side::buy, 5, 10, OrderType::limit, 8));

  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, EventType::fill);
  EXPECT_EQ(engine_.book().pool().live_count(), 0U);
}

TEST(EngineStp, CancelOldestRemovesTheRestingOrderAndLetsTheAggressorContinue) {
  using OldestEngine = Engine<CancelOldest, 4096>;

  OldestEngine::Config config;
  config.price =
      PriceConfig{.tick_size = TICK_SIZE, .price_scale = 10000, .base_price = BASE_PRICE};
  config.arena_capacity = 1024;
  config.max_cold_levels_per_side = 32;
  config.initial_center = Ticks{0};

  OldestEngine engine(config);
  Ring ring;

  const auto run = [&engine, &ring](const Command& command) {
    ring.clear();
    engine.submit(command, ring);
    std::vector<ExecutionEvent> events;
    ExecutionEvent event;
    while (ring.pop(event)) {
      events.push_back(event);
    }
    return events;
  };

  ASSERT_EQ(run(add(1, Side::sell, 5, 10, OrderType::limit, 7)).size(), 1U);
  ASSERT_EQ(run(add(2, Side::sell, 6, 10, OrderType::limit, 8)).size(), 1U);

  // Participant 7 buys through both. Its own resting order at 5 is removed, and
  // the aggressor carries on to trade with participant 8 at 6.
  const std::vector<ExecutionEvent> events = run(add(3, Side::buy, 6, 10, OrderType::limit, 7));

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].type, EventType::cancelled);
  EXPECT_EQ(events[0].reason, RejectReason::self_trade);
  EXPECT_EQ(events[0].taker_id, OrderId{1});

  EXPECT_EQ(events[1].type, EventType::fill);
  EXPECT_EQ(events[1].maker_id, OrderId{2});
  EXPECT_EQ(events[1].price, Ticks{6});

  EXPECT_EQ(engine.book().pool().live_count(), 0U);
}

// ---------------------------------------------------------------------------
// Rejections and bookkeeping
// ---------------------------------------------------------------------------

TEST_F(EngineFixture, RejectsDuplicateAndUnknownIds) {
  ASSERT_EQ(submit(add(1, Side::buy, 0, 10)).size(), 1U);

  const std::vector<ExecutionEvent> duplicate = submit(add(1, Side::buy, 0, 10));
  ASSERT_EQ(duplicate.size(), 1U);
  EXPECT_EQ(duplicate[0].reason, RejectReason::duplicate_order);

  const std::vector<ExecutionEvent> unknown = submit(cancel(99));
  ASSERT_EQ(unknown.size(), 1U);
  EXPECT_EQ(unknown[0].reason, RejectReason::unknown_order);

  EXPECT_EQ(engine_.book().pool().live_count(), 1U);
}

TEST_F(EngineFixture, RejectsOffTickPricesRatherThanRounding) {
  Command command = add(1, Side::buy, 0, 10);
  command.price = Price{BASE_PRICE + 50};  // half a tick

  const std::vector<ExecutionEvent> events = submit(command);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].reason, RejectReason::off_tick);

  // Rounding would silently change a customer's price, which is worse than
  // refusing the order.
  EXPECT_EQ(engine_.book().pool().live_count(), 0U);
}

TEST_F(EngineFixture, SequenceNumbersAreContiguousAcrossCommands) {
  ASSERT_EQ(submit(add(1, Side::sell, 5, 10)).size(), 1U);
  ASSERT_EQ(submit(add(2, Side::sell, 5, 10)).size(), 1U);

  const std::vector<ExecutionEvent> events = submit(add(3, Side::buy, 5, 20));

  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(events[0].sequence, Sequence{3});
  EXPECT_EQ(events[1].sequence, Sequence{4});
  EXPECT_EQ(engine_.next_sequence(), Sequence{5});
}

// The determinism claim, checked rather than asserted in prose. Two engines fed
// the same commands must agree exactly, which is the property the differential
// test and the replay harness both depend on.
TEST(EngineDeterminism, IdenticalInputProducesIdenticalOutput) {
  const auto run = [](std::vector<ExecutionEvent>& out) {
    TestEngine engine(make_config());
    Ring ring;

    for (std::uint64_t id = 1; id <= 200; ++id) {
      const Side side = (id % 2U) == 0U ? Side::buy : Side::sell;
      const auto tick = static_cast<std::int32_t>((id % 11U)) - 5;
      const OrderType type = (id % 7U) == 0U ? OrderType::immediate_or_cancel : OrderType::limit;

      ring.clear();
      engine.submit(add(id, side, tick, 5 + (id % 13U), type, static_cast<ParticipantId>(id % 3U)),
                    ring);

      ExecutionEvent event;
      while (ring.pop(event)) {
        out.push_back(event);
      }
    }
  };

  std::vector<ExecutionEvent> first;
  std::vector<ExecutionEvent> second;
  run(first);
  run(second);

  ASSERT_EQ(first.size(), second.size());
  ASSERT_GT(first.size(), 100U) << "the workload should have produced plenty of events";

  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].type, second[i].type) << "event " << i;
    EXPECT_EQ(first[i].taker_id, second[i].taker_id) << "event " << i;
    EXPECT_EQ(first[i].maker_id, second[i].maker_id) << "event " << i;
    EXPECT_EQ(first[i].quantity, second[i].quantity) << "event " << i;
    EXPECT_EQ(first[i].price, second[i].price) << "event " << i;
    EXPECT_EQ(first[i].sequence, second[i].sequence) << "event " << i;
  }
}

}  // namespace
}  // namespace ob
