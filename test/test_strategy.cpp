#include <cmath>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "ob/book.hpp"
#include "ob/types.hpp"

#include "strategy/market_maker.hpp"
#include "strategy/pnl.hpp"
#include "strategy/queue_position.hpp"

namespace {

using ob::Side;
using ob::Ticks;
using ob::strategy::Fill;
using ob::strategy::MarketMaker;
using ob::strategy::MarketMakerConfig;
using ob::strategy::PnlAccount;
using ob::strategy::QueueEstimator;
using ob::strategy::StrategyOrder;
using ob::strategy::VenueEvent;

StrategyOrder make_order(Side side, std::int32_t price, std::uint64_t size) {
  StrategyOrder order;
  order.id = ob::OrderId{42};
  order.side = side;
  order.price = Ticks{price};
  order.remaining = size;
  order.original_size = size;
  return order;
}

VenueEvent execution(Side side, std::int32_t price, std::uint64_t quantity) {
  VenueEvent event;
  event.kind = VenueEvent::Kind::execution;
  event.side = side;
  event.price = Ticks{price};
  event.quantity = quantity;
  return event;
}

VenueEvent cancellation(Side side, std::int32_t price, std::uint64_t quantity) {
  VenueEvent event;
  event.kind = VenueEvent::Kind::cancel;
  event.side = side;
  event.price = Ticks{price};
  event.quantity = quantity;
  return event;
}

// ---------------------------------------------------------------------------
// Queue position and the fill model.
// ---------------------------------------------------------------------------

TEST(StrategyQueue, ExecutionConsumesTheQueueAheadBeforeReachingUs) {
  QueueEstimator queue;
  queue.on_insert(make_order(Side::buy, 100, 100), 500);

  // 300 of the 500 ahead trade. Nothing reaches us.
  const std::vector<Fill> none = queue.on_venue_event(execution(Side::buy, 100, 300));
  EXPECT_TRUE(none.empty());
  ASSERT_EQ(queue.orders().size(), 1U);
  EXPECT_DOUBLE_EQ(queue.orders()[0].ahead, 200.0);
  EXPECT_EQ(queue.orders()[0].remaining, 100U);
}

TEST(StrategyQueue, OnlyTheExcessOverTheQueueAheadFillsUs) {
  QueueEstimator queue;
  queue.on_insert(make_order(Side::buy, 100, 100), 200);

  // 250 trade against a queue with 200 ahead of us, so 50 reach us.
  const std::vector<Fill> fills = queue.on_venue_event(execution(Side::buy, 100, 250));
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].quantity, 50U);
  EXPECT_FALSE(fills[0].from_trade_through);
  EXPECT_EQ(queue.orders()[0].remaining, 50U);
  EXPECT_DOUBLE_EQ(queue.orders()[0].ahead, 0.0);
}

TEST(StrategyQueue, AFillIsCappedAtOurRemainingSize) {
  QueueEstimator queue;
  queue.on_insert(make_order(Side::buy, 100, 100), 0);

  const std::vector<Fill> fills = queue.on_venue_event(execution(Side::buy, 100, 10000));
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].quantity, 100U);
  EXPECT_EQ(queue.orders()[0].remaining, 0U);
}

TEST(StrategyQueue, TradingThroughOurPriceFillsUsForTheTradedQuantity) {
  QueueEstimator queue;
  // A bid at 100 with a large queue ahead. It would never fill at its own price
  // here, but a trade at 99 means a seller accepted a worse price than ours.
  queue.on_insert(make_order(Side::buy, 100, 100), 100000);

  const std::vector<Fill> fills = queue.on_venue_event(execution(Side::buy, 99, 40));
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_EQ(fills[0].quantity, 40U);
  EXPECT_TRUE(fills[0].from_trade_through);
}

TEST(StrategyQueue, TradingAtABetterPriceThanOursDoesNotFillUs) {
  QueueEstimator queue;
  queue.on_insert(make_order(Side::buy, 100, 100), 0);

  // A bid at 101 is better than ours and gets hit first. We learn nothing.
  const std::vector<Fill> fills = queue.on_venue_event(execution(Side::buy, 101, 500));
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(queue.orders()[0].remaining, 100U);
}

TEST(StrategyQueue, TheOppositeSideNeverFillsUs) {
  QueueEstimator queue;
  queue.on_insert(make_order(Side::buy, 100, 100), 0);

  const std::vector<Fill> fills = queue.on_venue_event(execution(Side::sell, 100, 500));
  EXPECT_TRUE(fills.empty());
}

// The mirror of the trade-through rule, which is easy to get backwards because
// "worse" reverses with the side.
TEST(StrategyQueue, TradeThroughForASellIsAHigherPrice) {
  QueueEstimator queue;
  queue.on_insert(make_order(Side::sell, 100, 100), 100000);

  EXPECT_TRUE(queue.on_venue_event(execution(Side::sell, 99, 40)).empty());

  const std::vector<Fill> fills = queue.on_venue_event(execution(Side::sell, 101, 40));
  ASSERT_EQ(fills.size(), 1U);
  EXPECT_TRUE(fills[0].from_trade_through);
}

TEST(StrategyQueue, CancelsAreSplitUniformlyBetweenAheadAndBehind) {
  QueueEstimator queue;
  StrategyOrder order = make_order(Side::buy, 100, 100);
  queue.on_insert(order, 300);

  // Give the estimator a level of 300 ahead, 100 ours, 100 behind.
  ob::Book<>::Config config;
  config.price = ob::PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = 0};
  config.arena_capacity = 64;
  ob::Book<> book(config);

  ob::AddRequest ahead_order;
  ahead_order.id = ob::OrderId{1};
  ahead_order.side = Side::buy;
  ahead_order.price = Ticks{100};
  ahead_order.quantity = ob::Quantity{500};
  ASSERT_EQ(book.add(ahead_order), ob::AddStatus::ok);

  queue.refresh_behind(book);
  // 500 total at the level, 100 of it is nominally ours, so 400 others split
  // into 300 ahead and 100 behind.
  ASSERT_EQ(queue.orders().size(), 1U);
  EXPECT_DOUBLE_EQ(queue.orders()[0].ahead, 300.0);
  EXPECT_DOUBLE_EQ(queue.orders()[0].behind, 100.0);

  // 40 cancelled from a queue that is 300/400 ahead of us, so 30 came from
  // ahead under the uniform assumption.
  static_cast<void>(queue.on_venue_event(cancellation(Side::buy, 100, 40)));
  EXPECT_DOUBLE_EQ(queue.orders()[0].ahead, 270.0);
}

TEST(StrategyQueue, ACancelNeverFillsUs) {
  QueueEstimator queue;
  queue.on_insert(make_order(Side::buy, 100, 100), 0);

  const std::vector<Fill> fills = queue.on_venue_event(cancellation(Side::buy, 100, 5000));
  EXPECT_TRUE(fills.empty());
  EXPECT_EQ(queue.orders()[0].remaining, 100U);
}

// The decision recorded in DESIGN.md, "The crossed book, and why the strategy
// tolerates one". A passive venue add that crosses our quote is not a fill, and
// there is deliberately no VenueEvent kind that could make it one.
TEST(StrategyQueue, APassiveAddCrossingOurQuoteProducesNoFill) {
  QueueEstimator queue;
  queue.on_insert(make_order(Side::sell, 100, 100), 0);

  VenueEvent add;
  add.kind = VenueEvent::Kind::none;
  add.side = Side::buy;
  add.price = Ticks{105};  // crosses our ask at 100
  add.quantity = 1000;

  EXPECT_TRUE(queue.on_venue_event(add).empty());
  EXPECT_EQ(queue.orders()[0].remaining, 100U);
}

TEST(StrategyQueue, TheBookIsAuthoritativeWhenTheEstimateExceedsIt) {
  QueueEstimator queue;
  queue.on_insert(make_order(Side::buy, 100, 100), 10000);

  ob::Book<>::Config config;
  config.price = ob::PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = 0};
  config.arena_capacity = 64;
  ob::Book<> book(config);

  // The book says the level is empty. The estimate said 10000 rest ahead.
  queue.refresh_behind(book);
  EXPECT_DOUBLE_EQ(queue.orders()[0].ahead, 0.0);
  EXPECT_DOUBLE_EQ(queue.orders()[0].behind, 0.0);
}

// ---------------------------------------------------------------------------
// P&L.
// ---------------------------------------------------------------------------

// The claim the whole attribution rests on. If this does not hold exactly then
// the three components are not a decomposition, they are three unrelated numbers
// printed next to each other.
TEST(StrategyPnl, AttributionIsExact) {
  ob::strategy::FeeSchedule fees;
  PnlAccount pnl(fees);

  pnl.mark(100.0, 0);

  Fill buy;
  buy.side = Side::buy;
  buy.price = Ticks{99};
  buy.quantity = 100;
  pnl.on_fill(buy, true);

  pnl.mark(102.0, 1000);

  Fill sell;
  sell.side = Side::sell;
  sell.price = Ticks{103};
  sell.quantity = 60;
  pnl.on_fill(sell, true);

  pnl.mark(101.0, 2000);

  const double sum = pnl.spread_pnl() + pnl.inventory_pnl() + pnl.fee_pnl();
  EXPECT_DOUBLE_EQ(pnl.total_pnl(), sum);
}

TEST(StrategyPnl, BuyingBelowMidIsPositiveSpreadCapture) {
  PnlAccount pnl(ob::strategy::FeeSchedule{});
  pnl.mark(100.0, 0);

  Fill buy;
  buy.side = Side::buy;
  buy.price = Ticks{98};
  buy.quantity = 50;
  pnl.on_fill(buy, true);

  // Bought 50 at 98 against a mid of 100, so two ticks of edge on 50 shares.
  EXPECT_DOUBLE_EQ(pnl.spread_pnl(), 100.0);
  EXPECT_EQ(pnl.position(), 50);
}

TEST(StrategyPnl, InventoryPnlIsPositionTimesMidChange) {
  PnlAccount pnl(ob::strategy::FeeSchedule{});
  pnl.mark(100.0, 0);

  Fill buy;
  buy.side = Side::buy;
  buy.price = Ticks{100};
  buy.quantity = 10;
  pnl.on_fill(buy, true);

  pnl.mark(105.0, 1000);
  EXPECT_DOUBLE_EQ(pnl.inventory_pnl(), 50.0);

  pnl.mark(95.0, 2000);
  EXPECT_DOUBLE_EQ(pnl.inventory_pnl(), -50.0);
}

TEST(StrategyPnl, AMakerRebateIsCreditedAndATakerFeeCharged) {
  ob::strategy::FeeSchedule fees;
  fees.maker_rebate_per_share = -0.002;
  fees.taker_fee_per_share = 0.003;

  PnlAccount maker(fees);
  maker.mark(100.0, 0);
  Fill fill;
  fill.side = Side::buy;
  fill.price = Ticks{100};
  fill.quantity = 1000;
  maker.on_fill(fill, true);
  EXPECT_DOUBLE_EQ(maker.fee_pnl(), 2.0);

  PnlAccount taker(fees);
  taker.mark(100.0, 0);
  taker.on_fill(fill, false);
  EXPECT_DOUBLE_EQ(taker.fee_pnl(), -3.0);
}

TEST(StrategyPnl, MarkoutsResolveAtTheirHorizonAndCarryTheRightSign) {
  PnlAccount pnl(ob::strategy::FeeSchedule{});
  pnl.mark(100.0, 0);

  Fill buy;
  buy.side = Side::buy;
  buy.price = Ticks{100};
  buy.quantity = 10;
  buy.timestamp_ns = 0;
  pnl.on_fill(buy, true);

  // Before the horizon, nothing is resolved.
  pnl.mark(101.0, ob::strategy::MARKOUT_100MS_NS / 2);
  EXPECT_EQ(pnl.markout_100ms().all.fills, 0U);

  // At the horizon the mid is 2 above the fill price, on 10 shares bought.
  pnl.mark(102.0, ob::strategy::MARKOUT_100MS_NS);
  EXPECT_EQ(pnl.markout_100ms().all.fills, 1U);
  EXPECT_DOUBLE_EQ(pnl.markout_100ms().buy.total, 20.0);
  EXPECT_DOUBLE_EQ(pnl.markout_100ms().all.per_share(), 2.0);

  // A sale is the mirror: a mid above the sale price is a loss.
  EXPECT_EQ(pnl.markout_1s().all.fills, 0U);
  EXPECT_EQ(pnl.unresolved_markouts(), 1U);
}

TEST(StrategyPnl, ASellMarkoutIsNegativeWhenTheMarketRises) {
  PnlAccount pnl(ob::strategy::FeeSchedule{});
  pnl.mark(100.0, 0);

  Fill sell;
  sell.side = Side::sell;
  sell.price = Ticks{100};
  sell.quantity = 10;
  sell.timestamp_ns = 0;
  pnl.on_fill(sell, true);

  pnl.mark(103.0, ob::strategy::MARKOUT_100MS_NS);
  EXPECT_DOUBLE_EQ(pnl.markout_100ms().sell.total, -30.0);
}

// ---------------------------------------------------------------------------
// The market maker.
// ---------------------------------------------------------------------------

TEST(StrategyMaker, QuotesStraddleTheMidAtTheConfiguredOffset) {
  MarketMakerConfig config;
  config.quote_offset_ticks = 2;
  config.quote_size = 100;
  config.position_limit = 1000;
  MarketMaker maker(config);

  const auto intent = maker.desired(Ticks{100}, Ticks{110}, 0);
  ASSERT_TRUE(intent.want_bid);
  ASSERT_TRUE(intent.want_ask);
  EXPECT_EQ(intent.bid_price.raw(), 103);
  EXPECT_EQ(intent.ask_price.raw(), 107);
}

TEST(StrategyMaker, LongInventoryShiftsBothQuotesDown) {
  MarketMakerConfig config;
  config.quote_offset_ticks = 2;
  config.quote_size = 100;
  config.position_limit = 1000;
  config.inventory_skew_ticks = 4.0;
  MarketMaker maker(config);

  const auto flat = maker.desired(Ticks{100}, Ticks{110}, 0);
  const auto lengthy = maker.desired(Ticks{100}, Ticks{110}, 500);

  EXPECT_LT(lengthy.bid_price.raw(), flat.bid_price.raw());
  EXPECT_LT(lengthy.ask_price.raw(), flat.ask_price.raw());
}

TEST(StrategyMaker, ShortInventoryShiftsBothQuotesUp) {
  MarketMakerConfig config;
  config.quote_offset_ticks = 2;
  config.quote_size = 100;
  config.position_limit = 1000;
  config.inventory_skew_ticks = 4.0;
  MarketMaker maker(config);

  const auto flat = maker.desired(Ticks{100}, Ticks{110}, 0);
  const auto shorted = maker.desired(Ticks{100}, Ticks{110}, -500);

  EXPECT_GT(shorted.bid_price.raw(), flat.bid_price.raw());
  EXPECT_GT(shorted.ask_price.raw(), flat.ask_price.raw());
}

// The bug this pins: testing the limit against the current position rather than
// the position a full fill would produce lets a 100 share quote placed at 999 of
// a 1000 limit settle the book at 1099.
TEST(StrategyMaker, TheLimitAccountsForTheSizeOfTheQuoteItself) {
  MarketMakerConfig config;
  config.quote_offset_ticks = 1;
  config.quote_size = 100;
  config.position_limit = 1000;
  MarketMaker maker(config);

  const auto near_limit = maker.desired(Ticks{100}, Ticks{110}, 950);
  EXPECT_FALSE(near_limit.want_bid);
  EXPECT_TRUE(near_limit.want_ask);

  const auto at_limit = maker.desired(Ticks{100}, Ticks{110}, 900);
  EXPECT_TRUE(at_limit.want_bid);
}

TEST(StrategyMaker, NoQuotesWithoutATwoSidedMarket) {
  MarketMaker maker(MarketMakerConfig{});

  const auto no_bid = maker.desired(std::nullopt, Ticks{110}, 0);
  EXPECT_FALSE(no_bid.want_bid);
  EXPECT_FALSE(no_bid.want_ask);

  const auto crossed = maker.desired(Ticks{110}, Ticks{100}, 0);
  EXPECT_FALSE(crossed.want_bid);
  EXPECT_FALSE(crossed.want_ask);
}

TEST(StrategyMaker, QuotesNeverCrossEachOther) {
  MarketMakerConfig config;
  config.quote_offset_ticks = 0;
  config.quote_size = 100;
  config.position_limit = 1000;
  MarketMaker maker(config);

  // A zero offset against a one tick wide market would put both quotes on the
  // same price. The intent must be empty rather than self crossing.
  const auto intent = maker.desired(Ticks{100}, Ticks{101}, 0);
  if (intent.want_bid && intent.want_ask) {
    EXPECT_LT(intent.bid_price.raw(), intent.ask_price.raw());
  }
}

}  // namespace
