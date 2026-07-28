#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "ob/book.hpp"
#include "ob/engine.hpp"
#include "ob/events.hpp"

// Microbenchmarks for the book and engine hot operations.
//
// Methodology, because the number is worthless without it:
//
// One benchmark iteration is one book operation, so the reported time is directly
// the per-operation cost with no batch arithmetic to get wrong. The difficulty
// with that framing is that add and cancel both change the size of the book, so a
// long run either exhausts the arena or empties it. Rather than pausing the timer
// on every iteration, which costs more than the operation being measured, these
// benchmarks rebuild the book only when it runs out, under PauseTiming. At the
// default arena that happens roughly once per eighteen thousand iterations, so its
// contribution to the reported mean stays far below the measurement noise.
//
// The book is heap allocated because it holds megabytes of level array, and it is
// rebuilt rather than reset so that no benchmark inherits another's cache state.
//
// The case set is complete as of Phase 4: the nine operations the phase specifies
// are all covered, and two of the additions exist to correct measurements Phase 1
// got wrong rather than to add new ones.
//
// Workload sizes are held at or below the arena rather than at some round power of
// two, because a benchmark that exhausts the arena measures the rebuild path. The
// queue position arms use 128 levels of 64 orders and the bitmap pair uses 512
// levels of 16, both landing at 8 192 live orders, which is also close to the
// 8 842 peak a real day of QQQ reaches.

namespace {

using ob::AddRequest;
using ob::AddStatus;
using ob::Book;
using ob::CancelNewest;
using ob::CancelStatus;
using ob::Command;
using ob::CommandType;
using ob::DEFAULT_BAND_LEVELS;
using ob::Engine;
using ob::ExecutionEvent;
using ob::OrderId;
using ob::OrderType;
using ob::PriceConfig;
using ob::Quantity;
using ob::Side;
using ob::Ticks;
using ob::Timestamp;

using BenchBook = Book<DEFAULT_BAND_LEVELS>;
using BenchEngine = Engine<CancelNewest, DEFAULT_BAND_LEVELS>;

// The library default, so these figures describe the configuration that actually
// ships rather than one chosen for the benchmark.
//
// It is derived from measured depth: a full day of QQQ peaks at 8 842 live orders,
// so this carries 1.85x headroom. Going higher crosses an id map bucket boundary
// and costs 13 percent on every add, which is why it stops here. See the Config
// comment in book.hpp for that and for why capacity leads and cache residency
// follows.
//
// This file has now had the number wrong in both directions, which is why the
// sweep below exists. The first version used a 2^20 slot arena, putting 40 MiB of
// arena and 24 MiB of id map against an 8 MiB L3, so every figure was a DRAM
// latency wearing an operation's name. The second overcorrected to 2^16, which fit
// L2 but was eight times more headroom than any measured book needed.
// bm_add_by_arena_size sweeps the parameter so the sensitivity stays published
// rather than hidden.
constexpr std::uint32_t ARENA_CAPACITY = 16384;

// Kept well inside the band so that no rebase and no cold path operation can
// contaminate a measurement of the hot path.
constexpr std::int32_t PRICE_SPREAD = 512;

std::unique_ptr<BenchBook> make_book_with_arena(std::uint32_t arena_capacity) {
  BenchBook::Config config;
  config.price = PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = 1000000};
  config.arena_capacity = arena_capacity;
  config.initial_center = Ticks{0};
  return std::make_unique<BenchBook>(config);
}

std::unique_ptr<BenchBook> make_book() {
  return make_book_with_arena(ARENA_CAPACITY);
}

AddRequest request_for(std::uint64_t id, std::int32_t price) {
  AddRequest request;
  request.id = OrderId{id};
  request.side = Side::buy;
  request.price = Ticks{price};
  request.quantity = Quantity{100};
  request.timestamp = Timestamp{id};
  return request;
}

// Add where the target level already holds orders, which is the common case: the
// level bitmap is untouched and the work is a tail append plus two aggregate
// updates.
void bm_add_existing_level(benchmark::State& state) {
  std::unique_ptr<BenchBook> book = make_book();
  std::uint64_t id = 1;

  // Seed every level this benchmark will touch so that no iteration pays for an
  // empty-to-occupied transition.
  for (std::int32_t price = 0; price < PRICE_SPREAD; ++price) {
    benchmark::DoNotOptimize(book->add(request_for(id++, price)));
  }

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    const auto price = static_cast<std::int32_t>(id % PRICE_SPREAD);
    if (book->add(request_for(id, price)) != AddStatus::ok) {
      state.PauseTiming();
      book = make_book();
      id = 1;
      for (std::int32_t seed = 0; seed < PRICE_SPREAD; ++seed) {
        benchmark::DoNotOptimize(book->add(request_for(id++, seed)));
      }
      state.ResumeTiming();
    }
    ++id;
  }
}

// Add followed immediately by cancel of the same order, which is the best case
// this book has: the level goes empty to occupied and back so both bitmap
// transitions are included, and the arena free list is LIFO so every iteration
// reuses the slot the previous one released.
//
// Naming note. This was originally called bm_add_empty_level and presented as the
// cost of an add that has to touch the occupancy bitmap. It is not that, and the
// measurement said so: it came out faster than a plain add into an existing level,
// which is impossible if it is doing strictly more work. The reason is that
// cancelling immediately keeps the book at one live order, so the arena, the id
// map, and the single level all stay in L1, while the plain add benchmark grows
// the book until the arena fills. The two are not comparable and the name implied
// they were. Isolating the bitmap transition properly needs the paired
// same-working-set design that Phase 4 builds.
void bm_add_cancel_round_trip_hot(benchmark::State& state) {
  std::unique_ptr<BenchBook> book = make_book();
  std::uint64_t id = 1;

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    const auto price = static_cast<std::int32_t>(id % PRICE_SPREAD);

    if (book->add(request_for(id, price)) != AddStatus::ok) {
      state.PauseTiming();
      book = make_book();
      id = 1;
      state.ResumeTiming();
      continue;
    }

    // Inside the timed region on purpose: this benchmark reports the cost of the
    // pair, not of the add alone.
    benchmark::DoNotOptimize(book->cancel(OrderId{id}));
    ++id;
  }
}

// Cancel from the head of a populated level, which is the case a real feed
// produces most often: the front of the queue is what trades and what gets pulled.
void bm_cancel_head(benchmark::State& state) {
  std::unique_ptr<BenchBook> book = make_book();
  std::uint64_t next_id = 1;
  std::uint64_t oldest = 1;

  const auto refill = [&book, &next_id, &oldest]() {
    book = make_book();
    next_id = 1;
    oldest = 1;
    for (std::uint64_t i = 0; i < ARENA_CAPACITY - 1U; ++i) {
      const auto price = static_cast<std::int32_t>(next_id % PRICE_SPREAD);
      benchmark::DoNotOptimize(book->add(request_for(next_id, price)));
      ++next_id;
    }
  };

  refill();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (book->cancel(OrderId{oldest}) != CancelStatus::ok) {
      state.PauseTiming();
      refill();
      state.ResumeTiming();
      continue;
    }
    ++oldest;
  }
}

// Lookup in isolation: one Fibonacci hash, one masked index, and a probe. This is
// the operation that would be a pointer chase with std::unordered_map, and it is
// the first thing every cancel does.
void bm_lookup(benchmark::State& state) {
  const std::unique_ptr<BenchBook> book = make_book();

  constexpr std::uint64_t LIVE_ORDERS = 8192;
  for (std::uint64_t id = 1; id <= LIVE_ORDERS; ++id) {
    const auto price = static_cast<std::int32_t>(id % PRICE_SPREAD);
    benchmark::DoNotOptimize(book->add(request_for(id, price)));
  }

  std::uint64_t id = 1;
  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    benchmark::DoNotOptimize(book->find_order(OrderId{id}));
    id = (id % LIVE_ORDERS) + 1U;
  }

  state.counters["map_load_factor"] = book->id_map().load_factor();
  state.counters["mean_probes"] = book->id_map().mean_probe_count();
}

// Best price query in isolation: three dependent loads and three countr_zero or
// bit_width instructions, independent of how far the occupied levels are spread.
void bm_best_price(benchmark::State& state) {
  const std::unique_ptr<BenchBook> book = make_book();

  std::uint64_t id = 1;
  for (std::int32_t price = 0; price < PRICE_SPREAD; ++price) {
    benchmark::DoNotOptimize(book->add(request_for(id++, price)));
  }

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    // Forces a reload of the book each iteration. Without it the query is loop
    // invariant and the compiler hoists it, leaving an empty loop to measure.
    benchmark::ClobberMemory();
    benchmark::DoNotOptimize(book->best_bid());
  }
}

// ---------------------------------------------------------------------------
// The occupancy bitmap transition, isolated properly.
//
// This pair replaces the Phase 1 attempt, which was not a valid comparison and
// said so in its own naming note. That attempt put an add into an occupied level
// against an add-then-cancel that kept the book at a single live order, so one arm
// ran against tens of thousands of orders and the other against one. The arms had
// entirely different working sets, and the transition cost was nowhere in the
// difference between them.
//
// These two arms are identical in everything that costs time except the property
// under test. Both hold the book at a constant and equal size, both perform
// exactly one add and one cancel per iteration, both reuse the same arena slot
// because the free list is LIFO, and both walk the same toggle levels in the same
// order. The only difference is the sentinel: the excluded arm rests one order at
// every toggle level, so the level is already occupied when the probe add arrives
// and still occupied after the probe cancel, and no bit ever changes. The included
// arm has no sentinel, so every iteration sets a bit on the add and clears it on
// the cancel.
//
// The difference between the two arms is the cost of the transition. Neither
// figure means anything alone, and neither should be quoted alone.
// ---------------------------------------------------------------------------

// Toggle levels sit above the seeded background so the two ranges never interact.
constexpr std::int32_t TOGGLE_SPREAD = 256;

// Background depth per level, so that both arms measure against a book of
// realistic size rather than one that fits entirely in L1.
constexpr std::uint32_t BACKGROUND_DEPTH = 16;

void bitmap_transition_arm(benchmark::State& state, bool seed_sentinels) {
  std::unique_ptr<BenchBook> book = make_book();
  std::uint64_t id = 1;

  for (std::int32_t price = 0; price < PRICE_SPREAD; ++price) {
    for (std::uint32_t depth = 0; depth < BACKGROUND_DEPTH; ++depth) {
      benchmark::DoNotOptimize(book->add(request_for(id++, price)));
    }
  }

  if (seed_sentinels) {
    for (std::int32_t offset = 0; offset < TOGGLE_SPREAD; ++offset) {
      benchmark::DoNotOptimize(book->add(request_for(id++, PRICE_SPREAD + offset)));
    }
  } else {
    // The same number of orders, placed in the background instead, so that the
    // two arms hold identical live counts and the sentinel is the only remaining
    // difference between them.
    for (std::int32_t offset = 0; offset < TOGGLE_SPREAD; ++offset) {
      benchmark::DoNotOptimize(book->add(request_for(id++, offset % PRICE_SPREAD)));
    }
  }

  // One probe id, added and cancelled every iteration, so the book size never
  // drifts and the arena hands back the same slot each time.
  const std::uint64_t probe_id = id;
  std::int32_t offset = 0;

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    benchmark::DoNotOptimize(book->add(request_for(probe_id, PRICE_SPREAD + offset)));
    benchmark::DoNotOptimize(book->cancel(OrderId{probe_id}));
    offset = (offset + 1) % TOGGLE_SPREAD;
  }
}

void bm_bitmap_transition_included(benchmark::State& state) {
  bitmap_transition_arm(state, false);
}

void bm_bitmap_transition_excluded(benchmark::State& state) {
  bitmap_transition_arm(state, true);
}

// ---------------------------------------------------------------------------
// Cancel by queue position.
//
// The claim under test is that cancel costs the same wherever the order sits in
// its level's queue, because the id map goes straight to the arena slot and the
// intrusive links splice it out without a walk. A design that stores each level as
// a list and searches it would show head, middle, and tail diverging sharply.
// These three arms are the evidence for or against that claim.
// ---------------------------------------------------------------------------

enum class QueuePosition : std::uint8_t { head, middle, tail };

constexpr std::uint32_t QUEUE_DEPTH = 64;
constexpr std::int32_t QUEUE_LEVELS = 128;

// The exact sequence of ids to cancel so that every cancel removes the order then
// standing at the requested position of its level's queue.
//
// Precomputed rather than discovered inside the timed region, because locating the
// middle of an intrusive list requires a walk, and that walk is precisely the work
// this benchmark exists to show the book never performs. Measuring it would answer
// a different question. All three positions yield a sequence of the same length,
// and the timed loop consumes all three identically, one sequential vector element
// per iteration, so the arms stay comparable.
//
// The walk is round robin across levels rather than draining one level at a time,
// so that consecutive cancels touch different levels and no arm gets an
// artificially hot single level.
std::vector<std::uint64_t> cancel_sequence(QueuePosition position) {
  std::vector<std::vector<std::uint64_t>> live(static_cast<std::size_t>(QUEUE_LEVELS));

  for (std::int32_t level = 0; level < QUEUE_LEVELS; ++level) {
    const std::uint64_t base = (static_cast<std::uint64_t>(level) * QUEUE_DEPTH) + 1U;
    std::vector<std::uint64_t>& queue = live[static_cast<std::size_t>(level)];
    queue.reserve(QUEUE_DEPTH);
    for (std::uint32_t slot = 0; slot < QUEUE_DEPTH; ++slot) {
      queue.push_back(base + slot);
    }
  }

  std::vector<std::uint64_t> sequence;
  sequence.reserve(static_cast<std::size_t>(QUEUE_LEVELS) * QUEUE_DEPTH);

  for (std::uint32_t pass = 0; pass < QUEUE_DEPTH; ++pass) {
    for (std::int32_t level = 0; level < QUEUE_LEVELS; ++level) {
      std::vector<std::uint64_t>& queue = live[static_cast<std::size_t>(level)];
      std::size_t index = 0;
      switch (position) {
        case QueuePosition::head:
          index = 0;
          break;
        case QueuePosition::middle:
          index = queue.size() / 2U;
          break;
        case QueuePosition::tail:
          index = queue.size() - 1U;
          break;
      }
      sequence.push_back(queue[index]);
      queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
    }
  }

  return sequence;
}

void cancel_at_queue_position(benchmark::State& state, QueuePosition position) {
  const std::vector<std::uint64_t> sequence = cancel_sequence(position);

  std::unique_ptr<BenchBook> book;

  const auto rebuild = [&book]() {
    book = make_book();
    for (std::int32_t level = 0; level < QUEUE_LEVELS; ++level) {
      const std::uint64_t base = (static_cast<std::uint64_t>(level) * QUEUE_DEPTH) + 1U;
      for (std::uint32_t slot = 0; slot < QUEUE_DEPTH; ++slot) {
        benchmark::DoNotOptimize(book->add(request_for(base + slot, level)));
      }
    }
  };

  rebuild();
  std::size_t next = 0;

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (next == sequence.size()) {
      state.PauseTiming();
      rebuild();
      next = 0;
      state.ResumeTiming();
    }
    benchmark::DoNotOptimize(book->cancel(OrderId{sequence[next]}));
    ++next;
  }
}

void bm_cancel_queue_head(benchmark::State& state) {
  cancel_at_queue_position(state, QueuePosition::head);
}

void bm_cancel_queue_middle(benchmark::State& state) {
  cancel_at_queue_position(state, QueuePosition::middle);
}

void bm_cancel_queue_tail(benchmark::State& state) {
  cancel_at_queue_position(state, QueuePosition::tail);
}

// Modify down, the in-place quantity reduction that preserves queue position.
//
// Each iteration reduces a different order by one share, cycling a working set of
// MODIFY_ORDERS so that no single order stays pinned in L1 and the measurement
// reflects a realistic spread of touched lines. The new quantity is derived from a
// pass counter rather than tracked in a side array, which keeps the timed region
// down to the modify itself while still guaranteeing the strictly decreasing
// quantity that the reduction path requires. The rebuild guard is unreachable in
// any run of realistic length and exists so the benchmark cannot silently start
// measuring the increase path instead.
constexpr std::uint64_t MODIFY_START = 1U << 20;
constexpr std::uint64_t MODIFY_ORDERS = 4096;

void bm_modify_down(benchmark::State& state) {
  std::unique_ptr<BenchBook> book;

  const auto rebuild = [&book]() {
    book = make_book();
    for (std::uint64_t index = 0; index < MODIFY_ORDERS; ++index) {
      AddRequest request = request_for(index + 1U, static_cast<std::int32_t>(index % PRICE_SPREAD));
      request.quantity = Quantity{MODIFY_START};
      benchmark::DoNotOptimize(book->add(request));
    }
  };

  rebuild();

  std::uint64_t index = 0;
  std::uint64_t pass = 1;

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    benchmark::DoNotOptimize(book->modify(OrderId{index + 1U}, Quantity{MODIFY_START - pass}));
    ++index;
    if (index == MODIFY_ORDERS) {
      index = 0;
      ++pass;
      if (pass >= MODIFY_START) {
        state.PauseTiming();
        rebuild();
        pass = 1;
        state.ResumeTiming();
      }
    }
  }
}

// ---------------------------------------------------------------------------
// Aggressive orders that cross resting liquidity.
//
// These are the only benchmarks here that go through the engine rather than the
// book, because matching is the engine's job. The book is seeded with one resting
// sell per level and no bids, so an aggressive buy crosses a known, exact number
// of levels and nothing rests afterwards. Quantity is matched exactly to the
// liquidity consumed, so the aggressor is always fully filled and never becomes a
// resting bid that would change the shape of the book underneath the next
// iteration.
// ---------------------------------------------------------------------------

constexpr std::int32_t CROSS_LEVELS = 8192;
constexpr std::uint64_t CROSS_QTY = 100;

// The engine needs a sink exposing push. Counting is enough: it keeps the sink out
// of the measurement while still stopping the compiler from concluding that the
// events are unused.
struct CountingSink {
  std::uint64_t events = 0;

  void push(const ExecutionEvent& /*event*/) noexcept { ++events; }
};

BenchEngine::Config bench_engine_config() {
  BenchEngine::Config config;
  config.price = PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = 1000000};
  config.arena_capacity = ARENA_CAPACITY;
  config.initial_center = Ticks{0};
  return config;
}

Command resting_sell(std::uint64_t id, std::int32_t tick, const PriceConfig& price_config) {
  Command command;
  command.id = OrderId{id};
  command.type = CommandType::add;
  command.order_type = OrderType::limit;
  command.side = Side::sell;
  command.price = price_config.to_price(Ticks{tick});
  command.quantity = Quantity{CROSS_QTY};
  command.timestamp = Timestamp{id};
  return command;
}

Command aggressive_buy(std::uint64_t id,
                       std::int32_t limit_tick,
                       std::uint64_t quantity,
                       const PriceConfig& price_config) {
  Command command;
  command.id = OrderId{id};
  command.type = CommandType::add;
  command.order_type = OrderType::limit;
  command.side = Side::buy;
  command.price = price_config.to_price(Ticks{limit_tick});
  command.quantity = Quantity{quantity};
  command.timestamp = Timestamp{id};
  return command;
}

void cross_levels(benchmark::State& state, std::int32_t levels_per_cross) {
  const BenchEngine::Config config = bench_engine_config();

  std::unique_ptr<BenchEngine> engine;
  CountingSink sink;

  const auto rebuild = [&engine, &sink, &config]() {
    engine = std::make_unique<BenchEngine>(config);
    for (std::int32_t tick = 0; tick < CROSS_LEVELS; ++tick) {
      engine->submit(resting_sell(static_cast<std::uint64_t>(tick) + 1U, tick, config.price), sink);
    }
  };

  rebuild();

  std::int32_t next_tick = 0;
  std::uint64_t taker_id = static_cast<std::uint64_t>(CROSS_LEVELS) + 1U;

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (next_tick + levels_per_cross > CROSS_LEVELS) {
      state.PauseTiming();
      rebuild();
      next_tick = 0;
      state.ResumeTiming();
    }

    const std::int32_t limit_tick = next_tick + levels_per_cross - 1;
    engine->submit(aggressive_buy(taker_id,
                                  limit_tick,
                                  CROSS_QTY * static_cast<std::uint64_t>(levels_per_cross),
                                  config.price),
                   sink);
    ++taker_id;
    next_tick += levels_per_cross;
  }

  benchmark::DoNotOptimize(sink.events);
  state.counters["levels_crossed"] = levels_per_cross;
}

void bm_cross_one_level(benchmark::State& state) {
  cross_levels(state, 1);
}

void bm_cross_ten_levels(benchmark::State& state) {
  cross_levels(state, 10);
}

// The same add as bm_add_existing_level, swept over arena capacity.
//
// This exists because the first Phase 1 run reported roughly 54 ns for an
// add, which is far above what a handful of instructions against a flat array
// should cost. The suspicion was that the benchmark was measuring memory rather
// than the data structure: at the default 2^20 slot arena the resident set is
// about 40 MiB of arena plus 24 MiB of id map, which is several times this
// machine's 8 MiB L3, so nearly every add touches DRAM.
//
// Sweeping the capacity turns that suspicion into a measurement. The book's
// algorithmic work per add is identical at every point on this curve, so whatever
// the curve shows is cache behaviour and nothing else. This is also the shape of
// the experiment that Phase 4 uses in place of hardware cache-miss counters,
// which are unavailable on this host.
//
// The argument is the arena capacity. Live orders are held at a quarter of it so
// that no run ends up measuring arena exhaustion instead.
void bm_add_by_arena_size(benchmark::State& state) {
  const auto capacity = static_cast<std::uint32_t>(state.range(0));
  const std::uint64_t window = capacity / 4U;

  std::unique_ptr<BenchBook> book = make_book_with_arena(capacity);
  std::uint64_t id = 1;
  std::uint64_t oldest = 1;

  for (std::int32_t price = 0; price < PRICE_SPREAD; ++price) {
    benchmark::DoNotOptimize(book->add(request_for(id++, price)));
  }

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    const auto price = static_cast<std::int32_t>(id % PRICE_SPREAD);
    if (book->add(request_for(id, price)) != AddStatus::ok) {
      state.PauseTiming();
      book = make_book_with_arena(capacity);
      id = 1;
      oldest = 1;
      for (std::int32_t seed = 0; seed < PRICE_SPREAD; ++seed) {
        benchmark::DoNotOptimize(book->add(request_for(id++, seed)));
      }
      state.ResumeTiming();
      continue;
    }
    ++id;

    // Cancels are outside the reported comparison because every point on the
    // curve performs exactly the same number of them, so they shift the whole
    // curve without changing its shape.
    if (id - oldest > window) {
      benchmark::DoNotOptimize(book->cancel(OrderId{oldest}));
      ++oldest;
    }
  }

  state.counters["arena_MiB"] =
      static_cast<double>(capacity) * sizeof(ob::Order) / (1024.0 * 1024.0);
  state.counters["map_load"] = book->id_map().load_factor();
}

BENCHMARK(bm_add_existing_level);
BENCHMARK(bm_add_by_arena_size)
    ->Arg(1 << 12)
    ->Arg(1 << 14)  // the library default
    ->Arg(18432)    // one bucket above it, where the id map doubles
    ->Arg(1 << 16)
    ->Arg(1 << 18)
    ->Arg(1 << 20);
BENCHMARK(bm_add_cancel_round_trip_hot);
BENCHMARK(bm_cancel_head);
BENCHMARK(bm_lookup);
BENCHMARK(bm_best_price);

// Registered adjacently and deliberately: each pair or triple is only meaningful
// read against its siblings, never as an individual row.
BENCHMARK(bm_bitmap_transition_included);
BENCHMARK(bm_bitmap_transition_excluded);
BENCHMARK(bm_cancel_queue_head);
BENCHMARK(bm_cancel_queue_middle);
BENCHMARK(bm_cancel_queue_tail);
BENCHMARK(bm_modify_down);
BENCHMARK(bm_cross_one_level);
BENCHMARK(bm_cross_ten_levels);

}  // namespace

BENCHMARK_MAIN();
