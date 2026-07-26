#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ob::itch {

// Configuration for the synthetic ITCH 5.0 generator.
//
// Why this exists. Real TotalView-ITCH sample files are several gigabytes and sit
// behind a NASDAQ download. A repository whose tests and benchmarks only run for
// someone who has already fetched one of those is a repository whose tests nobody
// runs, including CI. This generator writes real ITCH 5.0 binary, so the parser and
// the replay driver read it through exactly the same code path as a real file, with
// no branch anywhere on where the bytes came from.
struct SyntheticConfig {
  std::string symbol = "TEST";
  std::uint64_t seed = 1;
  std::size_t message_count = 100000;

  // Scaled integer price with four implied decimals, matching the format. The
  // default is 100.0000.
  std::int64_t initial_price = 1000000;
  std::int64_t tick_size = 100;

  // Standard deviation of the mid's random walk, in ticks per message.
  //
  // The mid is carried as a fractional tick count and only rounded when a price is
  // written. An earlier version truncated each step to a whole number of ticks,
  // which meant any volatility below one tick rounded every single step to zero and
  // the mid never moved at all. The generated market was completely static and the
  // rebasing test that was supposed to catch that instead just reported no rebases.
  double volatility_ticks = 0.35;

  // Deterministic trend, in ticks per message, added to the random walk.
  //
  // A pure random walk drifts as the square root of the message count, so reaching
  // the band edge by chance takes an impractically long stream. A real trending
  // market has drift rather than just variance, so generating one needs this to be
  // a parameter rather than a hope.
  double drift_ticks_per_message = 0.0;

  // How far from the mid quotes are placed, in ticks. Wider means more occupied
  // levels and a sparser book.
  std::int32_t quote_depth_ticks = 12;

  std::uint32_t max_shares = 500;

  // Arrival rate, as nanoseconds between consecutive messages. Real ITCH
  // timestamps are nanoseconds since midnight and strictly non-decreasing.
  std::uint64_t nanos_between_messages = 1200;
  std::uint64_t start_nanos = 34200000000000;  // 09:30:00.000000000

  // Decoy symbols interleaved into the stream. Their presence is what proves the
  // replay driver's locate-code filtering actually filters, rather than the test
  // passing because every message happened to belong to the target symbol.
  std::uint16_t decoy_symbols = 2;

  // Message mix, as relative weights. The defaults are shaped after the observed
  // proportions in real TotalView data: adds and deletes dominate, executions are
  // a minority, and the informational types are rare.
  int weight_add = 42;
  int weight_add_with_mpid = 6;
  int weight_execute = 10;
  int weight_execute_with_price = 3;
  int weight_cancel = 7;
  int weight_delete = 24;
  int weight_replace = 5;
  int weight_trade = 2;
  int weight_cross = 1;
  int weight_broken = 1;
};

struct SyntheticSummary {
  std::size_t messages = 0;
  std::size_t bytes = 0;

  std::size_t system_events = 0;
  std::size_t stock_directories = 0;
  std::size_t adds = 0;
  std::size_t executions = 0;
  std::size_t cancels = 0;
  std::size_t deletes = 0;
  std::size_t replaces = 0;
  std::size_t trades = 0;
  std::size_t cross_trades = 0;
  std::size_t broken_trades = 0;
  std::size_t decoy_messages = 0;

  // Independently computed expectation of the final book, for validation. The
  // generator knows what it built, so replay can be checked against it without
  // trusting the parser to agree with itself.
  std::size_t expected_live_orders = 0;
  std::uint64_t expected_total_shares = 0;
};

// Generates a length prefixed ITCH 5.0 stream.
//
// The stream is semantically valid, not merely structurally valid, and that
// distinction is the whole difficulty. Executions, cancels, deletes and replaces
// only ever name orders that are actually live at that point in the stream, and an
// execution never exceeds the order's remaining size. A generator that emitted
// random order references would produce a file the parser reads happily and the
// replay driver rejects entirely, which would validate nothing.
//
// The book is also never crossed, because a real venue's book cannot be: bids are
// placed strictly below the mid and asks strictly above.
class SyntheticGenerator {
 public:
  explicit SyntheticGenerator(SyntheticConfig config);

  [[nodiscard]] std::vector<std::byte> generate();

  [[nodiscard]] const SyntheticSummary& summary() const noexcept { return summary_; }

 private:
  struct LiveOrder {
    std::uint64_t reference = 0;
    std::int64_t price = 0;
    std::uint32_t shares = 0;
    bool is_buy = false;
  };

  SyntheticConfig config_;
  SyntheticSummary summary_;
  std::vector<LiveOrder> live_;
};

}  // namespace ob::itch
