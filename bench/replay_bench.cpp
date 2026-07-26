#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <benchmark/benchmark.h>

#include "itch/parser.hpp"
#include "itch/replay.hpp"
#include "itch/synthetic.hpp"
#include "ob/book.hpp"

// Throughput of the ITCH pipeline, split so the parser and the book can be
// attributed separately.
//
// Two arms rather than one. The parse-only arm walks the stream and touches two
// header fields per message, which is what a filtered replay does to the large
// majority of messages it sees. The full arm parses and applies. The difference is
// the book's share of the work, and reporting only the combined figure would make
// it impossible to tell which half a change affected.
//
// Scope: this is throughput. Per-message latency percentiles into an HdrHistogram,
// broken out by message type, are Phase 4, along with core pinning and the
// environment capture that makes a published number trustworthy. Nothing here
// should be quoted as a headline figure.

namespace {

constexpr std::size_t BENCH_BAND = 4096;
using BenchBook = ob::Book<BENCH_BAND>;
using BenchDriver = ob::itch::ReplayDriver<BENCH_BAND>;

constexpr std::int64_t BENCH_BASE_PRICE = 1000000;
constexpr std::int64_t BENCH_TICK_SIZE = 100;
constexpr std::size_t BENCH_MESSAGES = 500000;

BenchBook::Config make_book_config() {
  BenchBook::Config config;
  config.price = ob::PriceConfig{.tick_size = BENCH_TICK_SIZE,
                                 .price_scale = ob::itch::PRICE_SCALE,
                                 .base_price = BENCH_BASE_PRICE};
  config.arena_capacity = 1U << 17;
  config.max_cold_levels_per_side = 256;
  config.initial_center = ob::Ticks{0};
  return config;
}

// Generated once and shared by both arms, so the two are measured on byte
// identical input and the comparison between them means something.
const std::vector<std::byte>& stream() {
  static const std::vector<std::byte> data = [] {
    ob::itch::SyntheticConfig config;
    config.symbol = "TEST";
    config.seed = 20260726;
    config.message_count = BENCH_MESSAGES;
    config.initial_price = BENCH_BASE_PRICE;
    config.tick_size = BENCH_TICK_SIZE;
    ob::itch::SyntheticGenerator generator(config);
    return generator.generate();
  }();
  return data;
}

// Framing plus two header field decodes per message, and nothing else. This is the
// floor: no replay can be faster than reading its own input.
void bm_parse_only(benchmark::State& state) {
  const std::vector<std::byte>& data = stream();

  std::size_t messages = 0;
  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    std::uint64_t checksum = 0;
    const ob::itch::ParseResult result =
        ob::itch::for_each_message(data, [&checksum](const ob::itch::MessageView& message) {
          // Reading the locate and the timestamp is exactly what a filtered replay
          // does before deciding to discard a message. Folding them into a checksum
          // stops the compiler eliminating the loads.
          checksum += message.stock_locate();
          checksum += message.timestamp();
        });
    benchmark::DoNotOptimize(checksum);
    messages = result.messages;
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(messages));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));
  state.counters["messages"] = static_cast<double>(messages);
}

// Parse and apply. The book is rebuilt each iteration, inside the timed region,
// because it has to be: replaying into a book that already holds the same orders
// would reject every add as a duplicate and measure the rejection path.
void bm_parse_and_replay(benchmark::State& state) {
  const std::vector<std::byte>& data = stream();

  std::size_t messages = 0;
  std::uint64_t applied = 0;
  std::uint64_t rebases = 0;

  for (auto unused : state) {
    benchmark::DoNotOptimize(unused);
    const std::unique_ptr<BenchBook> book = std::make_unique<BenchBook>(make_book_config());
    BenchDriver driver(*book, "TEST");
    const ob::itch::ParseResult result = ob::itch::replay_buffer(data, driver);
    benchmark::DoNotOptimize(book->pool().live_count());

    messages = result.messages;
    applied = driver.stats().messages_applied;
    rebases = book->rebase_count();
  }

  state.SetItemsProcessed(state.iterations() * static_cast<std::int64_t>(messages));
  state.SetBytesProcessed(state.iterations() * static_cast<std::int64_t>(data.size()));

  // Published alongside the throughput because they are the conditions that
  // produced it. A replay that filtered out most of its input, or that spent its
  // time rebasing, is not measuring what the headline number implies.
  state.counters["messages"] = static_cast<double>(messages);
  state.counters["applied"] = static_cast<double>(applied);
  state.counters["rebases"] = static_cast<double>(rebases);
}

BENCHMARK(bm_parse_only)->Unit(benchmark::kMillisecond);
BENCHMARK(bm_parse_and_replay)->Unit(benchmark::kMillisecond);

}  // namespace

BENCHMARK_MAIN();
