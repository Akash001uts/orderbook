#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <vector>

#include <gtest/gtest.h>

#include "alloc_counter.hpp"
#include "ob/book.hpp"
#include "ob/level_bitmap.hpp"
#include "ob/order_map.hpp"
#include "ob/order_pool.hpp"

namespace ob {
namespace {

// These justify the coverage note in alloc_counter.hpp. Every type this book
// allocates has alignment within max_align_t, so the over-aligned operator new
// overloads are unreachable and the counter cannot undercount. If one of these
// fails, the counter needs extending before its assertions mean anything again.
static_assert(alignof(Order) <= alignof(std::max_align_t));
static_assert(alignof(PriceLevel) <= alignof(std::max_align_t));
static_assert(alignof(std::uint64_t) <= alignof(std::max_align_t));
static_assert(alignof(ArenaIndex) <= alignof(std::max_align_t));

// A 4096 level band keeps the band-edge and rebase tests fast while exercising
// exactly the same code as the 65536 default: the bitmap still has three tiers,
// just one word wide in the middle.
using TestBook = Book<4096>;

constexpr std::size_t TEST_ARENA = 4096;

TestBook::Config make_config() {
  TestBook::Config config;
  config.price = PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = 1000000};
  config.arena_capacity = TEST_ARENA;
  config.max_cold_levels_per_side = 64;
  config.initial_center = Ticks{0};
  return config;
}

std::unique_ptr<TestBook> make_book() {
  return std::make_unique<TestBook>(make_config());
}

Ticks tick(std::int32_t value) {
  return Ticks{value};
}

AddRequest make_add(std::uint64_t id,
                    Side side,
                    std::int32_t price,
                    std::uint64_t quantity,
                    std::uint64_t timestamp = 1) {
  AddRequest request;
  request.id = OrderId{id};
  request.side = side;
  request.price = tick(price);
  request.quantity = Quantity{quantity};
  request.timestamp = Timestamp{timestamp};
  return request;
}

// Walks the intrusive list at a level and returns the ids in queue order. This is
// the function that proves FIFO priority, so it deliberately reads the links
// rather than trusting any aggregate.
std::vector<std::uint64_t> queue_at(const TestBook& book, Side side, std::int32_t price) {
  std::vector<std::uint64_t> ids;
  ArenaIndex index = book.first_order_at(side, tick(price));
  while (index != INVALID_INDEX) {
    ids.push_back(book.pool()[index].id.raw());
    index = book.pool()[index].next;
  }
  return ids;
}

// ---------------------------------------------------------------------------
// Order arena
// ---------------------------------------------------------------------------

TEST(OrderPool, AllocatesAndRecyclesSlots) {
  OrderPool pool(4);
  EXPECT_EQ(pool.capacity(), 4U);
  EXPECT_EQ(pool.live_count(), 0U);
  EXPECT_FALSE(pool.full());

  const ArenaIndex first = pool.allocate();
  const ArenaIndex second = pool.allocate();
  ASSERT_NE(first, INVALID_INDEX);
  ASSERT_NE(second, INVALID_INDEX);
  EXPECT_NE(first, second);
  EXPECT_EQ(pool.live_count(), 2U);

  pool.deallocate(first);
  EXPECT_EQ(pool.live_count(), 1U);

  // The free list is LIFO, so the slot just released is the next one handed out.
  // That is deliberate: it is the warmest slot in cache.
  EXPECT_EQ(pool.allocate(), first);
}

TEST(OrderPool, ExhaustsWithoutAllocatingOrThrowing) {
  OrderPool pool(3);
  EXPECT_NE(pool.allocate(), INVALID_INDEX);
  EXPECT_NE(pool.allocate(), INVALID_INDEX);
  EXPECT_NE(pool.allocate(), INVALID_INDEX);
  EXPECT_TRUE(pool.full());
  EXPECT_EQ(pool.allocate(), INVALID_INDEX);
  EXPECT_EQ(pool.live_count(), 3U);
}

TEST(OrderPool, GenerationParityTracksLiveness) {
  OrderPool pool(2);

  const ArenaIndex index = pool.allocate();
  EXPECT_TRUE(pool.is_live(index));

  pool.deallocate(index);
  EXPECT_FALSE(pool.is_live(index));

  // Handing the slot out again makes it live with a different generation, so a
  // stale index captured before the free is distinguishable from a fresh one.
  const ArenaIndex reused = pool.allocate();
  EXPECT_EQ(reused, index);
  EXPECT_TRUE(pool.is_live(reused));
  EXPECT_GE(pool[reused].generation, 3U);
}

TEST(OrderPool, OutOfRangeIndexIsNotLive) {
  const OrderPool pool(2);
  EXPECT_FALSE(pool.is_live(INVALID_INDEX));
  EXPECT_FALSE(pool.is_live(2));
  EXPECT_FALSE(pool.is_live(99));
}

// ---------------------------------------------------------------------------
// Open addressing id map
// ---------------------------------------------------------------------------

TEST(OrderIdMap, InsertFindErase) {
  OrderIdMap map(64);

  EXPECT_TRUE(map.insert(OrderId{7}, 3));
  EXPECT_EQ(map.find(OrderId{7}), 3U);
  EXPECT_EQ(map.size(), 1U);

  EXPECT_FALSE(map.insert(OrderId{7}, 9));
  EXPECT_EQ(map.find(OrderId{7}), 3U);

  EXPECT_TRUE(map.erase(OrderId{7}));
  EXPECT_EQ(map.find(OrderId{7}), INVALID_INDEX);
  EXPECT_FALSE(map.erase(OrderId{7}));
  EXPECT_EQ(map.size(), 0U);
}

TEST(OrderIdMap, RejectsReservedZeroKey) {
  OrderIdMap map(64);
  EXPECT_FALSE(map.insert(OrderId{0}, 1));
  EXPECT_EQ(map.find(OrderId{0}), INVALID_INDEX);
  EXPECT_FALSE(map.erase(OrderId{0}));
}

TEST(OrderIdMap, CapacityIsPowerOfTwoAtOrAboveTwiceExpected) {
  const OrderIdMap small(100);
  EXPECT_EQ(small.capacity(), 256U);

  const OrderIdMap exact(512);
  EXPECT_EQ(exact.capacity(), 1024U);
}

TEST(OrderIdMap, RefusesInsertBeyondHalfLoad) {
  OrderIdMap map(32);  // capacity 64, so 32 keys is the limit
  ASSERT_EQ(map.capacity(), 64U);

  std::uint32_t inserted = 0;
  for (std::uint64_t id = 1; id <= 200; ++id) {
    if (map.insert(OrderId{id}, static_cast<ArenaIndex>(id))) {
      ++inserted;
    }
  }

  EXPECT_EQ(inserted, map.size());
  EXPECT_LE(map.load_factor(), 0.55);
  EXPECT_GE(inserted, 32U);
}

// The property that backward shift deletion exists to guarantee. Tombstones would
// pass a simple insert-then-find test and still fail this one after enough churn.
TEST(OrderIdMap, EveryKeyStaysReachableAcrossHeavyChurn) {
  OrderIdMap map(2048);
  std::map<std::uint64_t, ArenaIndex> expected;

  std::uint64_t next_id = 1;
  for (int round = 0; round < 40; ++round) {
    for (int i = 0; i < 50; ++i) {
      const auto value = static_cast<ArenaIndex>(next_id % 1000U);
      if (map.insert(OrderId{next_id}, value)) {
        expected.emplace(next_id, value);
      }
      ++next_id;
    }

    // Erase every third key, which leaves probe sequences fragmented in exactly
    // the way that breaks a naive delete.
    std::vector<std::uint64_t> doomed;
    int counter = 0;
    for (const std::pair<const std::uint64_t, ArenaIndex>& entry : expected) {
      if (counter % 3 == 0) {
        doomed.push_back(entry.first);
      }
      ++counter;
    }
    for (const std::uint64_t id : doomed) {
      EXPECT_TRUE(map.erase(OrderId{id}));
      expected.erase(id);
    }

    ASSERT_EQ(map.size(), expected.size());
    for (const std::pair<const std::uint64_t, ArenaIndex>& entry : expected) {
      EXPECT_EQ(map.find(OrderId{entry.first}), entry.second) << "lost key " << entry.first;
    }
  }
}

TEST(OrderIdMap, ProbeCountStaysLowAtTheDesignLoadFactor) {
  OrderIdMap map(4096);
  for (std::uint64_t id = 1; id <= 4096; ++id) {
    ASSERT_TRUE(map.insert(OrderId{id}, static_cast<ArenaIndex>(id)));
  }

  EXPECT_NEAR(map.load_factor(), 0.5, 0.01);

  // The closed form for linear probing predicts about 2.5 probes at a load factor
  // of 0.5. Anything far above that means the hash is clustering.
  EXPECT_LT(map.mean_probe_count(), 3.0);
}

// ---------------------------------------------------------------------------
// Hierarchical bitmap
// ---------------------------------------------------------------------------

using TestBitmap = LevelBitmap<4096>;

TEST(LevelBitmap, EmptyReportsNone) {
  const TestBitmap bitmap;
  EXPECT_TRUE(bitmap.empty());
  EXPECT_EQ(bitmap.lowest_set(), TestBitmap::NONE);
  EXPECT_EQ(bitmap.highest_set(), TestBitmap::NONE);
  EXPECT_EQ(bitmap.next_set(0), TestBitmap::NONE);
  EXPECT_EQ(bitmap.prev_set(4095), TestBitmap::NONE);
  EXPECT_EQ(bitmap.popcount(), 0U);
}

TEST(LevelBitmap, SetAndQueryAcrossTierBoundaries) {
  TestBitmap bitmap;

  // 0 and 4095 are the extremes, 63 and 64 straddle a bottom-tier word, and 4032
  // is the first bit of the last word. These are the indices where the shift
  // arithmetic goes wrong if it is going to.
  for (const std::size_t level :
       {std::size_t{0}, std::size_t{63}, std::size_t{64}, std::size_t{4032}, std::size_t{4095}}) {
    bitmap.set(level);
    EXPECT_TRUE(bitmap.test(level)) << "level " << level;
  }

  EXPECT_FALSE(bitmap.empty());
  EXPECT_EQ(bitmap.popcount(), 5U);
  EXPECT_EQ(bitmap.lowest_set(), 0U);
  EXPECT_EQ(bitmap.highest_set(), 4095U);

  EXPECT_EQ(bitmap.next_set(0), 0U);
  EXPECT_EQ(bitmap.next_set(1), 63U);
  EXPECT_EQ(bitmap.next_set(64), 64U);
  EXPECT_EQ(bitmap.next_set(65), 4032U);
  EXPECT_EQ(bitmap.next_set(4096), TestBitmap::NONE);

  EXPECT_EQ(bitmap.prev_set(4095), 4095U);
  EXPECT_EQ(bitmap.prev_set(4094), 4032U);
  EXPECT_EQ(bitmap.prev_set(64), 64U);
  EXPECT_EQ(bitmap.prev_set(63), 63U);
  EXPECT_EQ(bitmap.prev_set(0), 0U);
}

TEST(LevelBitmap, ClearCascadesThroughTiers) {
  TestBitmap bitmap;
  bitmap.set(100);
  bitmap.set(101);

  bitmap.clear(100);
  EXPECT_FALSE(bitmap.test(100));
  EXPECT_TRUE(bitmap.test(101));
  EXPECT_EQ(bitmap.lowest_set(), 101U);

  // Clearing the last bit must propagate all the way to the top word, or the
  // bitmap reports occupancy for a level that no longer has any.
  bitmap.clear(101);
  EXPECT_TRUE(bitmap.empty());
  EXPECT_EQ(bitmap.lowest_set(), TestBitmap::NONE);
  EXPECT_EQ(bitmap.highest_set(), TestBitmap::NONE);
}

TEST(LevelBitmap, MatchesABruteForceReferenceOverEveryLevel) {
  TestBitmap bitmap;
  std::vector<bool> reference(4096, false);

  // A stride of 37 is coprime with 64, so set bits land at every offset within a
  // word rather than at a repeating position.
  for (std::size_t level = 0; level < 4096; level += 37) {
    bitmap.set(level);
    reference[level] = true;
  }

  for (std::size_t level = 0; level < 4096; ++level) {
    EXPECT_EQ(bitmap.test(level), reference[level]) << "level " << level;
  }

  std::size_t expected_low = 4096;
  std::size_t expected_high = 4096;
  std::size_t expected_count = 0;
  for (std::size_t level = 0; level < 4096; ++level) {
    if (!reference[level]) {
      continue;
    }
    if (expected_low == 4096) {
      expected_low = level;
    }
    expected_high = level;
    ++expected_count;
  }

  EXPECT_EQ(bitmap.lowest_set(), expected_low);
  EXPECT_EQ(bitmap.highest_set(), expected_high);
  EXPECT_EQ(bitmap.popcount(), expected_count);
}

// ---------------------------------------------------------------------------
// Book, basic operations
// ---------------------------------------------------------------------------

TEST(Book, EmptyBookHasNoBestPrices) {
  const std::unique_ptr<TestBook> book = make_book();

  EXPECT_FALSE(book->best_bid().has_value());
  EXPECT_FALSE(book->best_ask().has_value());
  EXPECT_EQ(book->depth_at(Side::buy, tick(0)), 0U);
  EXPECT_EQ(book->total_qty_at(Side::sell, tick(0)), Quantity{0});
  EXPECT_EQ(book->first_order_at(Side::buy, tick(0)), INVALID_INDEX);
}

TEST(Book, AddThenQuery) {
  const std::unique_ptr<TestBook> book = make_book();

  ASSERT_EQ(book->add(make_add(1, Side::buy, -5, 100)), AddStatus::ok);
  ASSERT_EQ(book->add(make_add(2, Side::sell, 5, 200)), AddStatus::ok);

  EXPECT_EQ(book->best_bid(), tick(-5));
  EXPECT_EQ(book->best_ask(), tick(5));
  EXPECT_EQ(book->depth_at(Side::buy, tick(-5)), 1U);
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(-5)), Quantity{100});
  EXPECT_EQ(book->total_qty_at(Side::sell, tick(5)), Quantity{200});
  EXPECT_EQ(book->pool().live_count(), 2U);
}

TEST(Book, RejectsInvalidAdds) {
  const std::unique_ptr<TestBook> book = make_book();

  EXPECT_EQ(book->add(make_add(0, Side::buy, 0, 100)), AddStatus::invalid_id);
  EXPECT_EQ(book->add(make_add(1, Side::buy, 0, 0)), AddStatus::zero_quantity);

  AddRequest oversized = make_add(2, Side::buy, 0, 1);
  oversized.quantity = Quantity{MAX_ORDER_SHARES + 1U};
  EXPECT_EQ(book->add(oversized), AddStatus::quantity_too_large);

  ASSERT_EQ(book->add(make_add(3, Side::buy, 0, 100)), AddStatus::ok);
  EXPECT_EQ(book->add(make_add(3, Side::sell, 10, 50)), AddStatus::duplicate_id);

  // A rejected add must leave nothing behind, on any of those paths.
  EXPECT_EQ(book->pool().live_count(), 1U);
  EXPECT_EQ(book->id_map().size(), 1U);
}

TEST(Book, BestPriceTracksTheTopOfEachSide) {
  const std::unique_ptr<TestBook> book = make_book();

  ASSERT_EQ(book->add(make_add(1, Side::buy, -10, 100)), AddStatus::ok);
  EXPECT_EQ(book->best_bid(), tick(-10));

  ASSERT_EQ(book->add(make_add(2, Side::buy, -3, 100)), AddStatus::ok);
  EXPECT_EQ(book->best_bid(), tick(-3));

  ASSERT_EQ(book->add(make_add(3, Side::buy, -20, 100)), AddStatus::ok);
  EXPECT_EQ(book->best_bid(), tick(-3));

  ASSERT_EQ(book->add(make_add(4, Side::sell, 30, 100)), AddStatus::ok);
  ASSERT_EQ(book->add(make_add(5, Side::sell, 12, 100)), AddStatus::ok);
  EXPECT_EQ(book->best_ask(), tick(12));

  ASSERT_EQ(book->cancel(OrderId{5}), CancelStatus::ok);
  EXPECT_EQ(book->best_ask(), tick(30));

  ASSERT_EQ(book->cancel(OrderId{4}), CancelStatus::ok);
  EXPECT_FALSE(book->best_ask().has_value());
  EXPECT_EQ(book->best_bid(), tick(-3));
}

TEST(Book, PreservesFifoWithinALevel) {
  const std::unique_ptr<TestBook> book = make_book();

  for (std::uint64_t id = 1; id <= 5; ++id) {
    ASSERT_EQ(book->add(make_add(id, Side::buy, 0, 10 * id, id)), AddStatus::ok);
  }

  EXPECT_EQ(queue_at(*book, Side::buy, 0), (std::vector<std::uint64_t>{1, 2, 3, 4, 5}));
  EXPECT_EQ(book->depth_at(Side::buy, tick(0)), 5U);
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(0)), Quantity{150});
}

TEST(Book, CancelUnlinksFromHeadMiddleAndTail) {
  const std::unique_ptr<TestBook> book = make_book();
  for (std::uint64_t id = 1; id <= 5; ++id) {
    ASSERT_EQ(book->add(make_add(id, Side::buy, 0, 10)), AddStatus::ok);
  }

  ASSERT_EQ(book->cancel(OrderId{1}), CancelStatus::ok);  // head
  EXPECT_EQ(queue_at(*book, Side::buy, 0), (std::vector<std::uint64_t>{2, 3, 4, 5}));

  ASSERT_EQ(book->cancel(OrderId{4}), CancelStatus::ok);  // middle
  EXPECT_EQ(queue_at(*book, Side::buy, 0), (std::vector<std::uint64_t>{2, 3, 5}));

  ASSERT_EQ(book->cancel(OrderId{5}), CancelStatus::ok);  // tail
  EXPECT_EQ(queue_at(*book, Side::buy, 0), (std::vector<std::uint64_t>{2, 3}));

  EXPECT_EQ(book->total_qty_at(Side::buy, tick(0)), Quantity{20});
  EXPECT_EQ(book->depth_at(Side::buy, tick(0)), 2U);
}

TEST(Book, CancelOfUnknownOrderChangesNothing) {
  const std::unique_ptr<TestBook> book = make_book();
  ASSERT_EQ(book->add(make_add(1, Side::buy, 0, 100)), AddStatus::ok);

  EXPECT_EQ(book->cancel(OrderId{999}), CancelStatus::unknown_order);
  EXPECT_EQ(book->cancel(OrderId{0}), CancelStatus::unknown_order);
  EXPECT_EQ(book->pool().live_count(), 1U);
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(0)), Quantity{100});
}

TEST(Book, LevelEmptiesAndRefills) {
  const std::unique_ptr<TestBook> book = make_book();

  ASSERT_EQ(book->add(make_add(1, Side::buy, 7, 100)), AddStatus::ok);
  EXPECT_EQ(book->best_bid(), tick(7));
  EXPECT_EQ(book->occupied_level_count(Side::buy), 1U);

  ASSERT_EQ(book->cancel(OrderId{1}), CancelStatus::ok);
  EXPECT_FALSE(book->best_bid().has_value());
  EXPECT_EQ(book->occupied_level_count(Side::buy), 0U);
  EXPECT_EQ(book->depth_at(Side::buy, tick(7)), 0U);
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(7)), Quantity{0});

  // Refilling must restore the bitmap bit and start a fresh queue, with none of
  // the previous occupant's links surviving.
  ASSERT_EQ(book->add(make_add(2, Side::buy, 7, 50)), AddStatus::ok);
  EXPECT_EQ(book->best_bid(), tick(7));
  EXPECT_EQ(book->occupied_level_count(Side::buy), 1U);
  EXPECT_EQ(queue_at(*book, Side::buy, 7), (std::vector<std::uint64_t>{2}));
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(7)), Quantity{50});
}

TEST(Book, ArenaExhaustionIsReportedNotFatal) {
  TestBook::Config config = make_config();
  config.arena_capacity = 4;
  TestBook book(config);

  for (std::uint64_t id = 1; id <= 4; ++id) {
    ASSERT_EQ(book.add(make_add(id, Side::buy, 0, 10)), AddStatus::ok);
  }

  EXPECT_EQ(book.add(make_add(5, Side::buy, 0, 10)), AddStatus::arena_exhausted);
  EXPECT_EQ(book.id_map().size(), 4U);
  EXPECT_EQ(book.total_qty_at(Side::buy, tick(0)), Quantity{40});
}

// ---------------------------------------------------------------------------
// Modify semantics
// ---------------------------------------------------------------------------

TEST(BookModify, QuantityDecreaseKeepsQueuePosition) {
  const std::unique_ptr<TestBook> book = make_book();
  for (std::uint64_t id = 1; id <= 3; ++id) {
    ASSERT_EQ(book->add(make_add(id, Side::buy, 0, 100)), AddStatus::ok);
  }

  EXPECT_EQ(book->modify(OrderId{1}, Quantity{40}), ModifyStatus::reduced_in_place);

  EXPECT_EQ(queue_at(*book, Side::buy, 0), (std::vector<std::uint64_t>{1, 2, 3}));
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(0)), Quantity{240});
  EXPECT_EQ(book->depth_at(Side::buy, tick(0)), 3U);
}

TEST(BookModify, QuantityIncreaseLosesQueuePosition) {
  const std::unique_ptr<TestBook> book = make_book();
  for (std::uint64_t id = 1; id <= 3; ++id) {
    ASSERT_EQ(book->add(make_add(id, Side::buy, 0, 100)), AddStatus::ok);
  }

  EXPECT_EQ(book->modify(OrderId{1}, Quantity{150}), ModifyStatus::requeued);

  // This is the exchange-correct outcome: the added shares cannot inherit the
  // priority the original shares earned.
  EXPECT_EQ(queue_at(*book, Side::buy, 0), (std::vector<std::uint64_t>{2, 3, 1}));
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(0)), Quantity{350});
  EXPECT_EQ(book->depth_at(Side::buy, tick(0)), 3U);
}

TEST(BookModify, RejectsAndNoOpsCorrectly) {
  const std::unique_ptr<TestBook> book = make_book();
  ASSERT_EQ(book->add(make_add(1, Side::buy, 0, 100)), AddStatus::ok);

  EXPECT_EQ(book->modify(OrderId{1}, Quantity{100}), ModifyStatus::unchanged);
  EXPECT_EQ(book->modify(OrderId{1}, Quantity{0}), ModifyStatus::zero_quantity);
  EXPECT_EQ(book->modify(OrderId{99}, Quantity{10}), ModifyStatus::unknown_order);
  EXPECT_EQ(book->modify(OrderId{1}, Quantity{MAX_ORDER_SHARES + 1U}),
            ModifyStatus::quantity_too_large);

  EXPECT_EQ(book->total_qty_at(Side::buy, tick(0)), Quantity{100});
}

TEST(BookModify, SingleOrderLevelSurvivesRequeue) {
  const std::unique_ptr<TestBook> book = make_book();
  ASSERT_EQ(book->add(make_add(1, Side::buy, 0, 100)), AddStatus::ok);

  // A requeue on a one-order level unlinks the only order and relinks it. If the
  // head and tail indices are not both maintained, the level ends up corrupt or
  // the bitmap ends up disagreeing with it.
  EXPECT_EQ(book->modify(OrderId{1}, Quantity{200}), ModifyStatus::requeued);
  EXPECT_EQ(queue_at(*book, Side::buy, 0), (std::vector<std::uint64_t>{1}));
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(0)), Quantity{200});
  EXPECT_EQ(book->best_bid(), tick(0));
  EXPECT_EQ(book->occupied_level_count(Side::buy), 1U);
}

// ---------------------------------------------------------------------------
// Band edges, rebasing, and the overflow cold path
// ---------------------------------------------------------------------------

TEST(BookBand, AcceptsOrdersAtBothBandExtremes) {
  const std::unique_ptr<TestBook> book = make_book();

  // The band spans 4096 levels centred on tick zero, so it covers -2048 to 2047.
  const std::int32_t low = book->band_base().raw();
  const std::int32_t high = low + static_cast<std::int32_t>(TestBook::BAND_LEVELS) - 1;

  EXPECT_TRUE(book->in_band(tick(low)));
  EXPECT_TRUE(book->in_band(tick(high)));
  EXPECT_FALSE(book->in_band(tick(low - 1)));
  EXPECT_FALSE(book->in_band(tick(high + 1)));

  ASSERT_EQ(book->add(make_add(1, Side::buy, low, 10)), AddStatus::ok);
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(low)), Quantity{10});

  // Touching an edge triggers a rebase, which moves the band under the order. The
  // order must still be findable at the same absolute price afterwards.
  EXPECT_GT(book->rebase_count(), 0U);
  EXPECT_EQ(book->best_bid(), tick(low));
  EXPECT_EQ(book->find_order(OrderId{1}), 0U);
}

TEST(BookBand, PricesOutsideTheBandReachTheColdPath) {
  const std::unique_ptr<TestBook> book = make_book();

  // Two bids far apart. The band cannot span both, so whichever falls outside
  // after recentring lives in cold storage, and both must still be queryable.
  ASSERT_EQ(book->add(make_add(1, Side::buy, 0, 10)), AddStatus::ok);
  ASSERT_EQ(book->add(make_add(2, Side::buy, 1000000, 20)), AddStatus::ok);

  EXPECT_EQ(book->total_qty_at(Side::buy, tick(0)), Quantity{10});
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(1000000)), Quantity{20});
  EXPECT_EQ(book->best_bid(), tick(1000000));
  EXPECT_GT(book->cold_operation_count(), 0U);

  // Cancelling out of cold storage must clean the level up rather than leave an
  // empty entry behind.
  ASSERT_EQ(book->cancel(OrderId{2}), CancelStatus::ok);
  EXPECT_EQ(book->best_bid(), tick(0));
  EXPECT_EQ(book->total_qty_at(Side::buy, tick(1000000)), Quantity{0});
}

TEST(BookBand, ColdCapIsReportedAsBandOverflow) {
  TestBook::Config config = make_config();
  config.max_cold_levels_per_side = 2;
  TestBook book(config);

  ASSERT_EQ(book.add(make_add(1, Side::buy, 0, 10)), AddStatus::ok);

  // Each of these is far enough away that recentring cannot bring it in, so each
  // consumes one cold level.
  ASSERT_EQ(book.add(make_add(2, Side::buy, 500000, 10)), AddStatus::ok);
  ASSERT_EQ(book.add(make_add(3, Side::buy, 900000, 10)), AddStatus::ok);

  const AddStatus overflow = book.add(make_add(4, Side::buy, 1300000, 10));
  EXPECT_EQ(overflow, AddStatus::band_overflow);

  // A band_overflow rejection must not leak an arena slot or a map entry.
  EXPECT_EQ(book.find_order(OrderId{4}), INVALID_INDEX);
  EXPECT_EQ(book.id_map().size(), book.pool().live_count());
}

// Regression test for a bug found while tracing the failure above.
//
// A rebase that must evict levels into cold storage can find the cold cap already
// full. The original code handled that by leaving the level where it was and
// carrying on with the shift, which silently misfiled every order in it: the level
// answered to whatever price its slot mapped to under the new band base. The fix
// counts the evictions before touching any state and abandons the whole rebase if
// they will not fit, so the band stays put and remains correct.
TEST(BookBand, RebaseIsAbandonedRatherThanLeftHalfDone) {
  TestBook::Config config = make_config();
  config.max_cold_levels_per_side = 1;
  TestBook book(config);

  // Three occupied levels mid band, far enough from either edge that none of these
  // adds triggers a rebase on its own.
  const std::vector<std::int32_t> prices{0, 1, 2};
  std::uint64_t id = 1;
  for (const std::int32_t price : prices) {
    ASSERT_EQ(book.add(make_add(id++, Side::buy, price, 10)), AddStatus::ok);
  }
  ASSERT_EQ(book.rebase_count(), 0U);

  // A bid outside the band takes the single available cold slot, and recentring on
  // it would push all three band levels out. That needs three cold slots against a
  // cap of one, so the rebase is infeasible.
  ASSERT_EQ(book.add(make_add(id, Side::buy, 5000, 10)), AddStatus::ok);

  EXPECT_GT(book.rebase_skipped_count(), 0U) << "the infeasible rebase should have been abandoned";
  EXPECT_EQ(book.rebase_count(), 0U) << "no rebase should have completed";

  // The whole point: every order is still at the price it was entered at.
  for (std::size_t i = 0; i < prices.size(); ++i) {
    const ArenaIndex index = book.find_order(OrderId{i + 1U});
    ASSERT_NE(index, INVALID_INDEX) << "lost order " << (i + 1U);
    EXPECT_EQ(book.pool()[index].price, tick(prices[i]));
    EXPECT_EQ(book.total_qty_at(Side::buy, tick(prices[i])), Quantity{10});
  }

  EXPECT_EQ(book.best_bid(), tick(5000));
  EXPECT_EQ(book.pool().live_count(), prices.size() + 1U);
}

// The aggressive rebasing test: a market that trends hard in
// one direction, with a rolling window of live orders, checked after every step.
TEST(BookBand, RebasingPreservesEveryRestingOrderInATrendingMarket) {
  const std::unique_ptr<TestBook> book = make_book();

  std::map<std::int32_t, std::uint64_t> expected_qty;  // price to total shares
  std::map<std::uint64_t, std::int32_t> live;          // order id to price

  std::uint64_t next_id = 1;
  constexpr std::int32_t STEP = 3;
  constexpr std::size_t WINDOW = 40;

  for (std::int32_t step = 0; step < 4000; ++step) {
    const std::int32_t price = step * STEP;

    const std::uint64_t id = next_id++;
    ASSERT_EQ(book->add(make_add(id, Side::buy, price, 25)), AddStatus::ok)
        << "add failed at price " << price;
    live.emplace(id, price);
    expected_qty[price] += 25;

    // Retire the trailing edge, the way a real quoting participant would.
    if (live.size() > WINDOW) {
      const std::uint64_t oldest = live.begin()->first;
      const std::int32_t oldest_price = live.begin()->second;
      ASSERT_EQ(book->cancel(OrderId{oldest}), CancelStatus::ok);
      expected_qty[oldest_price] -= 25;
      if (expected_qty[oldest_price] == 0) {
        expected_qty.erase(oldest_price);
      }
      live.erase(oldest);
    }

    ASSERT_EQ(book->best_bid(), tick(price)) << "best bid wrong at step " << step;
  }

  EXPECT_GT(book->rebase_count(), 0U) << "the trend should have forced rebasing";
  EXPECT_EQ(book->pool().live_count(), live.size());
  EXPECT_EQ(book->id_map().size(), live.size());

  // Not one resting order may have been lost or misplaced by a rebase.
  for (const std::pair<const std::int32_t, std::uint64_t>& entry : expected_qty) {
    EXPECT_EQ(book->total_qty_at(Side::buy, tick(entry.first)), Quantity{entry.second})
        << "quantity wrong at price " << entry.first;
  }
  for (const std::pair<const std::uint64_t, std::int32_t>& entry : live) {
    const ArenaIndex index = book->find_order(OrderId{entry.first});
    ASSERT_NE(index, INVALID_INDEX) << "lost order " << entry.first;
    EXPECT_EQ(book->pool()[index].price, tick(entry.second));
  }
}

TEST(BookBand, RebasingSurvivesATrendReversal) {
  const std::unique_ptr<TestBook> book = make_book();
  std::map<std::uint64_t, std::int32_t> live;
  std::uint64_t next_id = 1;

  // Up hard, then back down through the starting point. A reversal exercises the
  // opposite shift direction, which is the half of the relocation loop that an
  // upward-only test never reaches.
  for (std::int32_t price = 0; price < 6000; price += 4) {
    const std::uint64_t id = next_id++;
    ASSERT_EQ(book->add(make_add(id, Side::sell, price, 10)), AddStatus::ok);
    live.emplace(id, price);
    if (live.size() > 30) {
      ASSERT_EQ(book->cancel(OrderId{live.begin()->first}), CancelStatus::ok);
      live.erase(live.begin());
    }
  }

  for (std::int32_t price = 6000; price > -6000; price -= 4) {
    const std::uint64_t id = next_id++;
    ASSERT_EQ(book->add(make_add(id, Side::sell, price, 10)), AddStatus::ok);
    live.emplace(id, price);
    if (live.size() > 30) {
      const std::uint64_t doomed = live.rbegin()->first;
      ASSERT_EQ(book->cancel(OrderId{doomed}), CancelStatus::ok);
      live.erase(doomed);
    }
  }

  EXPECT_GT(book->rebase_count(), 1U);
  EXPECT_EQ(book->pool().live_count(), live.size());
  for (const std::pair<const std::uint64_t, std::int32_t>& entry : live) {
    const ArenaIndex index = book->find_order(OrderId{entry.first});
    ASSERT_NE(index, INVALID_INDEX) << "lost order " << entry.first;
    EXPECT_EQ(book->pool()[index].price, tick(entry.second));
  }
}

// ---------------------------------------------------------------------------
// The allocation assertion
// ---------------------------------------------------------------------------

TEST(BookAllocation, InBandHotPathAllocatesNothingAcrossAMillionOperations) {
  if (!ob::testing::counting_is_active()) {
    GTEST_SKIP() << "operator new replacement is compiled out under ThreadSanitizer, so the "
                    "counter would read zero for the wrong reason";
  }

  TestBook::Config config = make_config();
  config.arena_capacity = 8192;
  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(config);

  // Warmup. Construction has already faulted in the arena, the id map, and the
  // level arrays; this additionally exercises every branch the measured loop will
  // take, so nothing below is a first-time code path either.
  std::uint64_t id = 1;
  for (; id <= 2000; ++id) {
    ASSERT_EQ(book->add(make_add(id, Side::buy, static_cast<std::int32_t>(id % 64), 10)),
              AddStatus::ok);
  }
  for (std::uint64_t victim = 1; victim <= 2000; ++victim) {
    ASSERT_EQ(book->cancel(OrderId{victim}), CancelStatus::ok);
  }

  constexpr std::uint64_t OPERATIONS = 1'000'000;
  constexpr std::uint64_t WINDOW = 1000;

  const ob::testing::AllocationGuard guard;

  std::uint64_t operations = 0;
  std::uint64_t oldest = id;
  while (operations < OPERATIONS) {
    const auto price = static_cast<std::int32_t>(id % 512U);
    if (book->add(make_add(id, Side::buy, price, 10)) != AddStatus::ok) {
      break;
    }
    ++id;
    ++operations;

    if (id - oldest > WINDOW) {
      if (book->cancel(OrderId{oldest}) != CancelStatus::ok) {
        break;
      }
      ++oldest;
      ++operations;
    }
  }

  EXPECT_EQ(operations, OPERATIONS) << "the loop exited early, so it proved nothing";
  EXPECT_EQ(guard.allocations(), 0U) << "the hot path allocated " << guard.bytes() << " bytes";
  EXPECT_EQ(book->rebase_count(), 0U) << "a rebase would invalidate this measurement";
  EXPECT_EQ(book->cold_operation_count(), 0U) << "a cold operation would invalidate this";
}

// The complement of the test above, and the reason it is worth writing: it states
// where the zero-allocation guarantee stops. A claim with no stated boundary is
// not a claim.
TEST(BookAllocation, ColdPathDoesAllocateAndSaysSo) {
  if (!ob::testing::counting_is_active()) {
    GTEST_SKIP() << "operator new replacement is compiled out under ThreadSanitizer";
  }

  const std::unique_ptr<TestBook> book = make_book();
  ASSERT_EQ(book->add(make_add(1, Side::buy, 0, 10)), AddStatus::ok);

  const ob::testing::AllocationGuard guard;
  ASSERT_EQ(book->add(make_add(2, Side::buy, 2000000, 10)), AddStatus::ok);

  EXPECT_GT(guard.allocations(), 0U)
      << "the cold path is std::map backed, so it must allocate; if it stopped, this test is stale";
}

// ---------------------------------------------------------------------------
// Footprint, reported rather than asserted, so the Phase 1 write-up has real numbers.
// ---------------------------------------------------------------------------

TEST(BookFootprint, ReportsMemoryAtTheDefaultBandSize) {
  Book<DEFAULT_BAND_LEVELS>::Config config;
  config.arena_capacity = 1U << 18;
  const std::unique_ptr<Book<DEFAULT_BAND_LEVELS>> book =
      std::make_unique<Book<DEFAULT_BAND_LEVELS>>(config);

  EXPECT_EQ(book->band_memory_bytes(), 2U * 65536U * 24U);
  EXPECT_EQ(book->bitmap_memory_bytes(), 2U * (1024U + 16U + 1U) * 8U);
  EXPECT_EQ(book->pool().memory_bytes(), (1U << 18U) * 40U);

  std::cout << "sizeof(Order)          " << sizeof(Order) << " bytes\n"
            << "sizeof(PriceLevel)     " << sizeof(PriceLevel) << " bytes\n"
            << "band levels            " << (book->band_memory_bytes() / 1024U) << " KiB\n"
            << "occupancy bitmaps      " << (book->bitmap_memory_bytes() / 1024U) << " KiB\n"
            << "order arena            " << (book->pool().memory_bytes() / 1024U) << " KiB\n"
            << "id map                 " << (book->id_map().memory_bytes() / 1024U) << " KiB\n"
            << "total                  " << (book->total_memory_bytes() / 1024U) << " KiB\n";
}

}  // namespace
}  // namespace ob
