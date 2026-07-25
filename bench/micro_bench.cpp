#include <cstdint>
#include <memory>

#include <benchmark/benchmark.h>

#include "ob/book.hpp"

// Microbenchmarks for the three Phase 1 hot operations.
//
// Methodology, because the number is worthless without it:
//
// One benchmark iteration is one book operation, so the reported time is directly
// the per-operation cost with no batch arithmetic to get wrong. The difficulty
// with that framing is that add and cancel both change the size of the book, so a
// long run either exhausts the arena or empties it. Rather than pausing the timer
// on every iteration, which costs more than the operation being measured, these
// benchmarks rebuild the book only when it runs out, under PauseTiming. With a
// 2^20 slot arena that happens roughly once per million iterations, so its
// contribution to the reported mean is far below the measurement noise.
//
// The book is heap allocated because it holds megabytes of level array, and it is
// rebuilt rather than reset so that no benchmark inherits another's cache state.
//
// These numbers are a first look for Phase 1, taken with whatever core the
// scheduler happened to provide. The disciplined harness, with core pinning,
// warmup, environment capture, and run-to-run variance, is Phase 4. Nothing here
// should be quoted as a headline figure.

namespace {

using ob::AddRequest;
using ob::AddStatus;
using ob::Book;
using ob::CancelStatus;
using ob::DEFAULT_BAND_LEVELS;
using ob::OrderId;
using ob::PriceConfig;
using ob::Quantity;
using ob::Side;
using ob::Ticks;
using ob::Timestamp;

using BenchBook = Book<DEFAULT_BAND_LEVELS>;

// 65536 live orders, which is a busy but realistic depth for a single liquid
// symbol. It is not chosen to flatter the numbers, and the first version of this
// file got it wrong in the other direction: a 2^20 slot arena puts 40 MiB of arena
// and 24 MiB of id map against an 8 MiB L3, so every reported figure was a DRAM
// latency rather than a property of the book. bm_add_by_arena_size below sweeps
// this parameter so that the sensitivity is published rather than hidden.
constexpr std::uint32_t ARENA_CAPACITY = 1U << 16;

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

  constexpr std::uint64_t LIVE_ORDERS = 1U << 16;
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
    benchmark::DoNotOptimize(book->best_bid());
  }
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
    ->Arg(1 << 14)
    ->Arg(1 << 16)
    ->Arg(1 << 18)
    ->Arg(1 << 20);
BENCHMARK(bm_add_cancel_round_trip_hot);
BENCHMARK(bm_cancel_head);
BENCHMARK(bm_lookup);
BENCHMARK(bm_best_price);

}  // namespace

BENCHMARK_MAIN();
