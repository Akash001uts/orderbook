#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "ob/engine.hpp"
#include "ob/events.hpp"
#include "ob/types.hpp"

#include "alloc_counter.hpp"
#include "reference_book.hpp"

// The comparative baseline: the flat book against the naive std::map book, on a
// workload that is identical down to the command.
//
// This is the highest value measurement in Phase 4 and it is nearly free, because
// the naive book already exists. test/reference_book.hpp is the oracle the
// differential test checks the engine against, so it is not a strawman written to
// lose: it is the implementation whose agreement with the fast one is what makes
// the correctness argument. Every choice in it is the opposite of the fast book's,
// which is exactly what makes it the right thing to measure against.
//
// Why the ratio is the publishable number. It is measured on one machine, in one
// process, with both arms subject to the same scheduler, the same turbo state, and
// the same cache pressure. Whatever the absolute figures are worth on an unpinned
// hybrid CPU, the two arms are worth the same, so what survives is the comparison.
// The absolute values are reported alongside so the reader can see them, with the
// coefficient of variation next to each.
//
// The mechanical explanation, which is what makes a ratio mean anything, comes
// from three instruments rather than from perf counters. Hardware counters are
// unavailable on every host this project runs on: Windows has no perf, WSL2 is a
// hypervisor guest whose /sys/bus/event_source/devices exposes no cpu source, and
// GitHub runners expose none either. The substitutes:
//
//   1. Allocation counts, reported by every benchmark below as a counter. These
//      are exact rather than sampled, and they are measured outside the timed
//      region so the counting itself never enters a reported time.
//   2. The working set sweep, which BENCHMARKS.md carries for the flat book and
//      which the sweep arm here extends across both implementations.
//   3. A structural pointer hop count, stated in BENCHMARKS.md per operation.
//
// Allocation is the largest single structural difference and the one that needs no
// interpretation. The reference book allocates a map node for every newly occupied
// level, a list node for every resting order, and an unordered_map node for every
// live id. The flat book allocates nothing at all once constructed.

namespace {

using ob::CancelNewest;
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
using ob::testing::AllocationGuard;
using ob::testing::counting_is_active;
using ob::testing::ReferenceBook;

using FlatEngine = Engine<CancelNewest, DEFAULT_BAND_LEVELS>;
using NaiveBook = ReferenceBook<CancelNewest>;

// 8 192 live orders against a 16 384 slot arena, which is matched to the workload
// rather than to the library default of 65 536.
//
// The default is sized for the deepest mainstream symbol measured, AAPL at 27 097
// live orders, so a caller who does not know their depth cannot lose orders.
// Measuring at the default would put 8 192 orders in a 2.5 MiB arena and report a
// DRAM latency rather than a property of the book. The order count itself is close
// to QQQ's real 8 842 peak, and levels are spread wide enough that neither
// implementation degenerates into a single hot level.
constexpr std::uint32_t BASELINE_ORDERS = 8192;
constexpr std::int32_t BASELINE_LEVELS = 512;
constexpr std::uint32_t ARENA_CAPACITY = 16384;
constexpr std::uint64_t CROSS_QTY = 100;

constexpr PriceConfig BASELINE_PRICES{
    .tick_size = 100, .price_scale = 10000, .base_price = 1000000};

FlatEngine::Config flat_config() {
  FlatEngine::Config config;
  config.price = BASELINE_PRICES;
  config.arena_capacity = ARENA_CAPACITY;
  config.initial_center = Ticks{0};
  return config;
}

// The engine takes a sink with push, the reference book appends to a vector.
// Wrapping the same vector means both implementations do identical work to store
// the events they produce, so event handling cannot tilt the comparison.
struct VectorSink {
  std::vector<ExecutionEvent>* out = nullptr;

  // Returns true: it grows rather than rejecting, so the engine's overflow policy
  // never fires here. The engine reads the result, so push must be bool.
  [[nodiscard]] bool push(const ExecutionEvent& event) const {
    out->push_back(event);
    return true;
  }
};

Command make_add(std::uint64_t id, std::int32_t tick, Side side, std::uint64_t quantity) {
  Command command;
  command.id = OrderId{id};
  command.type = CommandType::add;
  command.order_type = OrderType::limit;
  command.side = side;
  command.price = BASELINE_PRICES.to_price(Ticks{tick});
  command.quantity = Quantity{quantity};
  command.timestamp = Timestamp{id};
  return command;
}

Command make_cancel(std::uint64_t id) {
  Command command;
  command.id = OrderId{id};
  command.type = CommandType::cancel;
  command.timestamp = Timestamp{id};
  return command;
}

// Resting buys only, so nothing crosses and every add lands on the book. The tick
// walks a fixed set of levels rather than climbing, so neither implementation
// pays for a rebase and the reference book's map stays a constant size.
Command resting_add(std::uint64_t id) {
  return make_add(id, static_cast<std::int32_t>(id % BASELINE_LEVELS), Side::buy, CROSS_QTY);
}

// -------------------------------------------------------------------------
// Allocation measurement.
//
// Run outside every timed region, on a fresh instance, so that the operator new
// replacement never contributes to a reported time. Reported per operation.
// -------------------------------------------------------------------------

double flat_allocations_per_add() {
  std::vector<ExecutionEvent> events;
  events.reserve(BASELINE_ORDERS);
  VectorSink sink{&events};

  auto engine = std::make_unique<FlatEngine>(flat_config());

  const AllocationGuard guard;
  for (std::uint64_t id = 1; id <= BASELINE_ORDERS; ++id) {
    engine->submit(resting_add(id), sink);
  }
  const auto allocations = static_cast<double>(guard.allocations());

  return allocations / static_cast<double>(BASELINE_ORDERS);
}

double naive_allocations_per_add() {
  std::vector<ExecutionEvent> events;
  events.reserve(BASELINE_ORDERS);

  auto book = std::make_unique<NaiveBook>(BASELINE_PRICES);

  const AllocationGuard guard;
  for (std::uint64_t id = 1; id <= BASELINE_ORDERS; ++id) {
    book->submit(resting_add(id), events);
  }
  const auto allocations = static_cast<double>(guard.allocations());

  return allocations / static_cast<double>(BASELINE_ORDERS);
}

// The event vector is reserved before the guard in both cases, so its own growth
// is never counted against either implementation.
void report_allocations(benchmark::State& state, double per_operation) {
  if (!counting_is_active()) {
    // The operator new replacement is compiled out under ThreadSanitizer. Report
    // nothing rather than report a counter stuck at zero, which would read as
    // "allocates nothing" for both arms and quietly invert the finding.
    return;
  }
  state.counters["allocs_per_op"] = per_operation;
}

// -------------------------------------------------------------------------
// Add.
// -------------------------------------------------------------------------

void bm_add_flat(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);
  VectorSink sink{&events};

  std::unique_ptr<FlatEngine> engine;
  std::uint64_t id = 1;

  const auto rebuild = [&engine, &id]() {
    engine = std::make_unique<FlatEngine>(flat_config());
    id = 1;
  };

  rebuild();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (id > BASELINE_ORDERS) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    events.clear();
    engine->submit(resting_add(id), sink);
    ++id;
  }

  report_allocations(state, flat_allocations_per_add());
}

void bm_add_naive(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);

  std::unique_ptr<NaiveBook> book;
  std::uint64_t id = 1;

  const auto rebuild = [&book, &id]() {
    book = std::make_unique<NaiveBook>(BASELINE_PRICES);
    id = 1;
  };

  rebuild();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (id > BASELINE_ORDERS) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    events.clear();
    book->submit(resting_add(id), events);
    ++id;
  }

  report_allocations(state, naive_allocations_per_add());
}

// The same flat add, but delivering into a bounded EventRing rather than the
// always-accepting VectorSink. EventRing::push returns a runtime bool the compiler
// cannot fold to a constant, so the engine's overflow-check branch stays live here
// where it optimises away for VectorSink. The ring is sized well above the events a
// single add emits and drained each iteration, so it never overflows: this isolates
// the cost of the bool-returning sink contract for a bounded caller. It is
// not part of the published std::map comparison; it exists only for that comparison.
void bm_add_flat_bounded_sink(benchmark::State& state) {
  ob::EventRing<64> ring;

  std::unique_ptr<FlatEngine> engine;
  std::uint64_t id = 1;

  const auto rebuild = [&engine, &id]() {
    engine = std::make_unique<FlatEngine>(flat_config());
    id = 1;
  };

  rebuild();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (id > BASELINE_ORDERS) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    ring.clear();
    engine->submit(resting_add(id), ring);
    ++id;
  }
}

// -------------------------------------------------------------------------
// Cancel.
//
// This is where the two designs diverge most sharply and where the reference
// book's O(n) level walk is the whole story: it locates the order's level through
// an index, then searches the level's list linearly for the id. The flat book
// reaches the arena slot directly and splices the intrusive links.
// -------------------------------------------------------------------------

void bm_cancel_flat(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);
  VectorSink sink{&events};

  std::unique_ptr<FlatEngine> engine;
  std::uint64_t next = 1;

  const auto rebuild = [&engine, &sink, &events, &next]() {
    engine = std::make_unique<FlatEngine>(flat_config());
    for (std::uint64_t id = 1; id <= BASELINE_ORDERS; ++id) {
      events.clear();
      engine->submit(resting_add(id), sink);
    }
    next = 1;
  };

  rebuild();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (next > BASELINE_ORDERS) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    events.clear();
    engine->submit(make_cancel(next), sink);
    ++next;
  }
}

void bm_cancel_naive(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);

  std::unique_ptr<NaiveBook> book;
  std::uint64_t next = 1;

  const auto rebuild = [&book, &events, &next]() {
    book = std::make_unique<NaiveBook>(BASELINE_PRICES);
    for (std::uint64_t id = 1; id <= BASELINE_ORDERS; ++id) {
      events.clear();
      book->submit(resting_add(id), events);
    }
    next = 1;
  };

  rebuild();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (next > BASELINE_ORDERS) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    events.clear();
    book->submit(make_cancel(next), events);
    ++next;
  }
}

// -------------------------------------------------------------------------
// Best price query.
//
// The flat book reads a bitmap. The reference book asks a std::map for its first
// or last element, which is a tree descent. Both are cheap and neither is the
// headline, but the pair is worth publishing because it is the one operation
// where the naive structure is not obviously worse and the honest answer is that
// the gap is small.
// -------------------------------------------------------------------------

void bm_best_price_flat(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);
  VectorSink sink{&events};

  auto engine = std::make_unique<FlatEngine>(flat_config());
  for (std::uint64_t id = 1; id <= BASELINE_ORDERS; ++id) {
    events.clear();
    engine->submit(resting_add(id), sink);
  }

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    // A full memory barrier on every iteration, so the compiler cannot hoist the
    // query out of the loop. Without it the arm reports the cost of an empty loop,
    // which is not hypothetical: caching the best price made this read 0.277 ns,
    // below one cycle, and that impossibility is what gave it away. Escaping the
    // pointer alone was tried first and was not enough.
    benchmark::ClobberMemory();
    benchmark::DoNotOptimize(engine->book().best_bid());
  }
}

// The harness's own noise floor for this shape of loop.
//
// Not decoration. Once the flat book's best price query became a single load, the
// arm reported 0.26 ns, which is below one cycle and indistinguishable by eye from
// a benchmark that had been optimised away entirely. The only way to tell those
// apart is to measure a loop that provably does nothing and compare. If the query
// arm and this arm agree, the honest statement is that the query is below what
// this harness can resolve, not that it costs 0.26 ns.
void bm_query_noise_floor(benchmark::State& state) {
  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    benchmark::ClobberMemory();
  }
}

void bm_best_price_naive(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);

  auto book = std::make_unique<NaiveBook>(BASELINE_PRICES);
  for (std::uint64_t id = 1; id <= BASELINE_ORDERS; ++id) {
    events.clear();
    book->submit(resting_add(id), events);
  }

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    // Same barrier as the flat arm, so the two stay comparable.
    benchmark::ClobberMemory();
    benchmark::DoNotOptimize(book->best_bid());
  }
}

// -------------------------------------------------------------------------
// Matching.
//
// One resting sell per level, and an aggressive buy that consumes exactly one of
// them per iteration. Both implementations see the same commands in the same
// order and produce the same events, which the differential test already
// guarantees, so any difference in time is a difference in how the two structures
// find and remove the liquidity.
// -------------------------------------------------------------------------

constexpr std::int32_t MATCH_LEVELS = 8192;

void bm_match_flat(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);
  VectorSink sink{&events};

  std::unique_ptr<FlatEngine> engine;
  std::int32_t tick = 0;
  std::uint64_t taker = static_cast<std::uint64_t>(MATCH_LEVELS) + 1U;

  const auto rebuild = [&engine, &sink, &events, &tick]() {
    engine = std::make_unique<FlatEngine>(flat_config());
    for (std::int32_t level = 0; level < MATCH_LEVELS; ++level) {
      events.clear();
      engine->submit(make_add(static_cast<std::uint64_t>(level) + 1U, level, Side::sell, CROSS_QTY),
                     sink);
    }
    tick = 0;
  };

  rebuild();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (tick >= MATCH_LEVELS) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    events.clear();
    engine->submit(make_add(taker, tick, Side::buy, CROSS_QTY), sink);
    ++taker;
    ++tick;
  }
}

void bm_match_naive(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);

  std::unique_ptr<NaiveBook> book;
  std::int32_t tick = 0;
  std::uint64_t taker = static_cast<std::uint64_t>(MATCH_LEVELS) + 1U;

  const auto rebuild = [&book, &events, &tick]() {
    book = std::make_unique<NaiveBook>(BASELINE_PRICES);
    for (std::int32_t level = 0; level < MATCH_LEVELS; ++level) {
      events.clear();
      book->submit(make_add(static_cast<std::uint64_t>(level) + 1U, level, Side::sell, CROSS_QTY),
                   events);
    }
    tick = 0;
  };

  rebuild();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (tick >= MATCH_LEVELS) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    events.clear();
    book->submit(make_add(taker, tick, Side::buy, CROSS_QTY), events);
    ++taker;
    ++tick;
  }
}

// -------------------------------------------------------------------------
// Working set sweep, run on both implementations.
//
// This is the instrument that stands in for the cache counters no host here can
// provide. The algorithmic work per add must be identical at every point of the
// sweep and identical between the two arms, so that the entire shape of each
// curve, and the entire difference between the curves, is memory behaviour.
//
// The argument is the number of live orders. The flat book's arena is sized to
// match it, so that the sweep moves the working set rather than the headroom.
//
// The level count scales with the order count, holding the queue depth fixed at
// SWEEP_DEPTH. This is not cosmetic. The first version of this sweep held the
// level count fixed, which quietly changed the algorithmic work across the sweep
// for the naive arm: with a fixed 512 levels, a 1024 order book creates a new
// level on half of all adds while a 65536 order book creates one on 1 in 128, and
// a new level is a std::map node allocation. The naive curve consequently fell as
// the working set grew, which is not a cache effect and not believable as one.
// Holding the depth fixed makes the level creation rate constant at one in
// SWEEP_DEPTH for both arms, which is what the sweep needs to mean anything.
// -------------------------------------------------------------------------

constexpr std::int32_t SWEEP_DEPTH = 64;

std::int32_t sweep_levels(std::uint64_t live) {
  const auto levels = static_cast<std::int32_t>(live / static_cast<std::uint64_t>(SWEEP_DEPTH));
  return levels < 1 ? 1 : levels;
}

Command sweep_add(std::uint64_t id, std::int32_t levels) {
  return make_add(
      id, static_cast<std::int32_t>(id % static_cast<std::uint64_t>(levels)), Side::buy, CROSS_QTY);
}

void bm_sweep_flat(benchmark::State& state) {
  const auto live = static_cast<std::uint64_t>(state.range(0));
  const std::int32_t levels = sweep_levels(live);

  std::vector<ExecutionEvent> events;
  events.reserve(64);
  VectorSink sink{&events};

  FlatEngine::Config config = flat_config();
  config.arena_capacity = static_cast<std::uint32_t>(live * 2U);

  std::unique_ptr<FlatEngine> engine;
  std::uint64_t id = 1;

  const auto rebuild = [&engine, &config, &id]() {
    engine = std::make_unique<FlatEngine>(config);
    id = 1;
  };

  rebuild();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (id > live) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    events.clear();
    engine->submit(sweep_add(id, levels), sink);
    ++id;
  }

  state.counters["live_orders"] = static_cast<double>(live);
  state.counters["levels"] = levels;
}

void bm_sweep_naive(benchmark::State& state) {
  const auto live = static_cast<std::uint64_t>(state.range(0));
  const std::int32_t levels = sweep_levels(live);

  std::vector<ExecutionEvent> events;
  events.reserve(64);

  std::unique_ptr<NaiveBook> book;
  std::uint64_t id = 1;

  const auto rebuild = [&book, &id]() {
    book = std::make_unique<NaiveBook>(BASELINE_PRICES);
    id = 1;
  };

  rebuild();

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (id > live) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    events.clear();
    book->submit(sweep_add(id, levels), events);
    ++id;
  }

  state.counters["live_orders"] = static_cast<double>(live);
  state.counters["levels"] = levels;
}

// Registered in pairs, flat then naive, so that the console output reads as the
// comparison it is. No row here is meaningful without the row beside it.
BENCHMARK(bm_add_flat);
BENCHMARK(bm_add_naive);
BENCHMARK(bm_add_flat_bounded_sink);
BENCHMARK(bm_cancel_flat);
BENCHMARK(bm_cancel_naive);
BENCHMARK(bm_query_noise_floor);
BENCHMARK(bm_best_price_flat);
BENCHMARK(bm_best_price_naive);
BENCHMARK(bm_match_flat);
BENCHMARK(bm_match_naive);
BENCHMARK(bm_sweep_flat)->Arg(1 << 10)->Arg(1 << 12)->Arg(1 << 14)->Arg(1 << 16);
BENCHMARK(bm_sweep_naive)->Arg(1 << 10)->Arg(1 << 12)->Arg(1 << 14)->Arg(1 << 16);

}  // namespace

BENCHMARK_MAIN();
