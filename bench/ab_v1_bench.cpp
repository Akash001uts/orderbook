// Cross-version controlled benchmark driver.
//
// This one source is compiled unchanged against two engine trees, the engine before
// the public interface hardening and the engine after it, so the only difference between the two
// arms is the engine implementation the include path resolves. It uses only the
// public API both versions share (Engine, its Config, PriceConfig, Command, the
// strong integer types) plus its own sinks, so it does not depend on anything B
// added or removed. The point is to measure whether the bool-returning sink
// contract and the representability check cost anything on the hot flat paths, for both
// an always-accepting sink (where the overflow branch folds away) and a bounded one
// (where it stays live).
//
// The bounded sink deliberately has no [[nodiscard]] on push: the older engine
// discards the result with static_cast<void>, and a [[nodiscard]] there is not
// wanted for a benchmark-only type. It never overflows: it is sized well above the
// events one command emits and is drained every iteration.
//
// This is not the published std::map comparison and does not touch baseline.json.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "ob/engine.hpp"
#include "ob/events.hpp"
#include "ob/types.hpp"

namespace {

using ob::CancelNewest;
using ob::Command;
using ob::CommandType;
using ob::Engine;
using ob::ExecutionEvent;
using ob::OrderId;
using ob::OrderType;
using ob::Price;
using ob::PriceConfig;
using ob::Quantity;
using ob::Side;
using ob::Ticks;
using ob::Timestamp;

using BenchEngine = Engine<CancelNewest, 4096>;

constexpr std::int64_t BASE_PRICE = 1000000;
constexpr std::int64_t TICK_SIZE = 100;
constexpr std::int64_t PRICE_SCALE = 10000;
constexpr std::int32_t LEVELS = 512;
constexpr std::uint64_t ORDERS = 8192;
constexpr std::uint64_t CROSS_QTY = 100;
// Kept inside the 4096-level band (centered at 0, so [-2048, 2047]) so the match
// path measures the flat in-band matcher rather than the cold overflow map.
constexpr std::int32_t MATCH_LEVELS = 2000;

BenchEngine::Config bench_config() {
  BenchEngine::Config config;
  config.price =
      PriceConfig{.tick_size = TICK_SIZE, .price_scale = PRICE_SCALE, .base_price = BASE_PRICE};
  config.arena_capacity = 16384;
  config.max_cold_levels_per_side = 64;
  config.initial_center = Ticks{0};
  return config;
}

// Always-accepting sink: grows, never rejects. The engine's overflow branch folds
// away because push is provably true. The older engine discards the result with
// static_cast<void>, which suppresses [[nodiscard]] there, and the newer one reads
// it, so both versions compile against this.
struct GrowSink {
  std::vector<ExecutionEvent>* out = nullptr;

  [[nodiscard]] bool push(const ExecutionEvent& event) const {
    out->push_back(event);
    return true;
  }
};

// Bounded, non-overflowing sink: push returns a runtime bool the compiler cannot
// fold to a constant, so the newer engine's overflow branch stays live. Sized
// far above the events a single command emits and cleared each iteration. Not
// [[nodiscard]] because it is non-const; the older engine's static_cast<void>
// discard compiles regardless.
template <std::size_t Capacity>
struct BoundedSink {
  std::array<ExecutionEvent, Capacity> buffer{};
  std::size_t count = 0;

  bool push(const ExecutionEvent& event) {
    if (count == Capacity) {
      return false;
    }
    buffer[count++] = event;
    return true;
  }

  void clear() { count = 0; }
};

Command make_add(std::uint64_t id, std::int32_t tick, Side side, std::uint64_t quantity) {
  Command command;
  command.id = OrderId{id};
  command.type = CommandType::add;
  command.order_type = OrderType::limit;
  command.side = side;
  command.price = Price{BASE_PRICE + (static_cast<std::int64_t>(tick) * TICK_SIZE)};
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

Command resting_add(std::uint64_t id) {
  return make_add(id, static_cast<std::int32_t>(id % LEVELS), Side::buy, CROSS_QTY);
}

// -------------------------------------------------------------------------
// Add.
// -------------------------------------------------------------------------

template <typename Sink, typename Drain>
void run_add(benchmark::State& state, Sink& sink, Drain drain) {
  std::unique_ptr<BenchEngine> engine;
  std::uint64_t id = 1;
  const auto rebuild = [&engine, &id]() {
    engine = std::make_unique<BenchEngine>(bench_config());
    id = 1;
  };
  rebuild();
  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (id > ORDERS) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    drain();
    engine->submit(resting_add(id), sink);
    ++id;
  }
}

void bm_add_grow(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);
  GrowSink sink{&events};
  run_add(state, sink, [&events]() { events.clear(); });
}

void bm_add_bounded(benchmark::State& state) {
  BoundedSink<64> sink;
  run_add(state, sink, [&sink]() { sink.clear(); });
}

// -------------------------------------------------------------------------
// Cancel.
// -------------------------------------------------------------------------

template <typename Sink, typename Drain>
void run_cancel(benchmark::State& state, Sink& sink, Drain drain) {
  std::unique_ptr<BenchEngine> engine;
  std::uint64_t next = 1;
  const auto rebuild = [&engine, &sink, &drain, &next]() {
    engine = std::make_unique<BenchEngine>(bench_config());
    for (std::uint64_t id = 1; id <= ORDERS; ++id) {
      drain();
      engine->submit(resting_add(id), sink);
    }
    next = 1;
  };
  rebuild();
  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    if (next > ORDERS) {
      state.PauseTiming();
      rebuild();
      state.ResumeTiming();
    }
    drain();
    engine->submit(make_cancel(next), sink);
    ++next;
  }
}

void bm_cancel_grow(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);
  GrowSink sink{&events};
  run_cancel(state, sink, [&events]() { events.clear(); });
}

void bm_cancel_bounded(benchmark::State& state) {
  BoundedSink<64> sink;
  run_cancel(state, sink, [&sink]() { sink.clear(); });
}

// -------------------------------------------------------------------------
// Match, one resting order consumed per iteration.
// -------------------------------------------------------------------------

template <typename Sink, typename Drain>
void run_match(benchmark::State& state, Sink& sink, Drain drain) {
  std::unique_ptr<BenchEngine> engine;
  std::int32_t tick = 0;
  std::uint64_t taker = static_cast<std::uint64_t>(MATCH_LEVELS) + 1U;
  const auto rebuild = [&engine, &sink, &drain, &tick]() {
    engine = std::make_unique<BenchEngine>(bench_config());
    for (std::int32_t level = 0; level < MATCH_LEVELS; ++level) {
      drain();
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
    drain();
    engine->submit(make_add(taker, tick, Side::buy, CROSS_QTY), sink);
    ++taker;
    ++tick;
  }
}

void bm_match_grow(benchmark::State& state) {
  std::vector<ExecutionEvent> events;
  events.reserve(64);
  GrowSink sink{&events};
  run_match(state, sink, [&events]() { events.clear(); });
}

void bm_match_bounded(benchmark::State& state) {
  BoundedSink<64> sink;
  run_match(state, sink, [&sink]() { sink.clear(); });
}

// -------------------------------------------------------------------------
// Noise floor, a version-independent drift control (not a gated engine path).
// -------------------------------------------------------------------------

void bm_noise_floor(benchmark::State& state) {
  std::uint64_t counter = 0;
  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    benchmark::ClobberMemory();
    benchmark::DoNotOptimize(++counter);
  }
}

BENCHMARK(bm_add_grow);
BENCHMARK(bm_add_bounded);
BENCHMARK(bm_cancel_grow);
BENCHMARK(bm_cancel_bounded);
BENCHMARK(bm_match_grow);
BENCHMARK(bm_match_bounded);
BENCHMARK(bm_noise_floor);

}  // namespace

BENCHMARK_MAIN();
