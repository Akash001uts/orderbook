#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "differential_harness.hpp"
#include "ob/engine.hpp"
#include "ob/events.hpp"
#include "reference_book.hpp"

namespace ob {
namespace {

using testing::compare_events;
using testing::decode;
using testing::diff_engine_config;
using testing::diff_price_config;
using testing::DiffEngine;
using testing::DiffReference;
using testing::DiffRing;
using testing::encode;
using testing::Mismatch;
using testing::replay;
using testing::ReplayResult;
using testing::step;

// The same randomised command sequence is fed to the real engine and to the naive
// reference book, and full state equivalence is asserted after every command.
//
// Checking after every command rather than at the end is the whole design. A
// comparison at the end says something diverged somewhere in a million commands,
// which is nearly useless. A comparison after each one names the exact command,
// with a book small enough to read.
//
// This harness has been validated by mutation rather than assumed to work. Three
// deliberate bugs were injected and each was caught by a different one of its
// checks: a crossing predicate changed from <= to < was caught by the event
// stream at command 3, a partial fill that skipped the level aggregate was caught
// by the aggregate comparison at command 9, and a quantity reduction that
// requeued instead of holding its place was caught by the queue order comparison
// at command 399. A differential test that has never been shown to fail is a
// differential test nobody should trust.

// -----------------------------------------------------------------------------
// Command generation.
//
// The distribution matters as much as the volume. A generator that mostly emits
// non-crossing limit orders explores almost none of the matching engine, so these
// weights deliberately keep the book tight and the aggressive rate high.
// -----------------------------------------------------------------------------

struct GeneratorConfig {
  std::uint64_t seed = 1;

  // A narrow price range relative to the number of live orders is what forces
  // levels to be shared, queues to be deep, and crossings to be frequent.
  std::int32_t price_span = 20;

  // Participant count drives the self trade prevention rate. Few participants
  // means many self trades, which is unrealistic and exactly what a test wants.
  ParticipantId participants = 3;

  std::uint32_t max_quantity = 50;

  // When set, most orders are placed on the passive side of the mid: bids strictly
  // below it, asks strictly above. Uniform placement across a wide span does not
  // build a deep book, it builds a violently swept one, because a buy at the top
  // of the span crosses every ask beneath it. That is worth knowing and it is not
  // what a test of sparse-book behaviour wants, so this exists to produce depth.
  bool passive_bias = false;

  // Fraction of orders, out of 100, that are placed to cross when passive_bias is
  // on. Some crossing is essential or the matcher is never exercised at all.
  int aggressive_percent = 15;
};

// Weights out of 100, chosen so roughly a third of commands are aggressive and
// roughly a third remove liquidity, which keeps the book from growing without
// bound while keeping the matcher busy.
class CommandGenerator {
 public:
  explicit CommandGenerator(const GeneratorConfig& config) : config_(config), rng_(config.seed) {}

  [[nodiscard]] Command next(const std::vector<std::uint64_t>& live_ids) {
    const int roll = uniform(0, 99);

    if (roll < 34) {
      return make_add(OrderType::limit);
    }
    if (roll < 40) {
      return make_add(OrderType::market);
    }
    if (roll < 48) {
      return make_add(OrderType::immediate_or_cancel);
    }
    if (roll < 54) {
      return make_add(OrderType::fill_or_kill);
    }
    if (roll < 62) {
      return make_add(OrderType::post_only);
    }
    if (roll < 88) {
      return make_targeted(CommandType::cancel, live_ids);
    }
    return make_targeted(CommandType::modify, live_ids);
  }

 private:
  [[nodiscard]] int uniform(int low, int high) {
    return std::uniform_int_distribution<int>(low, high)(rng_);
  }

  [[nodiscard]] std::uint64_t uniform64(std::uint64_t low, std::uint64_t high) {
    return std::uniform_int_distribution<std::uint64_t>(low, high)(rng_);
  }

  [[nodiscard]] Command make_add(OrderType order_type) {
    Command command;
    command.type = CommandType::add;
    command.order_type = order_type;
    command.id = OrderId{next_id_++};
    command.side = uniform(0, 1) == 0 ? Side::buy : Side::sell;
    command.quantity = Quantity{uniform64(1, config_.max_quantity)};
    command.timestamp = Timestamp{timestamp_++};
    command.party = static_cast<ParticipantId>(uniform(1, static_cast<int>(config_.participants)));

    const std::int32_t tick = choose_tick(command.side);
    command.price = Price{testing::DIFF_BASE_PRICE +
                          (static_cast<std::int64_t>(tick) * testing::DIFF_TICK_SIZE)};
    return command;
  }

  [[nodiscard]] std::int32_t choose_tick(Side side) {
    if (!config_.passive_bias || uniform(0, 99) < config_.aggressive_percent) {
      return uniform(-config_.price_span, config_.price_span);
    }

    // Bids land in [-span, -1] and asks in [1, span], so a passive order on one
    // side can never cross a passive order on the other.
    const std::int32_t magnitude = uniform(1, config_.price_span);
    return side == Side::buy ? -magnitude : magnitude;
  }

  [[nodiscard]] Command make_targeted(CommandType type,
                                      const std::vector<std::uint64_t>& live_ids) {
    Command command;
    command.type = type;
    command.timestamp = Timestamp{timestamp_++};
    command.quantity = Quantity{uniform64(1, config_.max_quantity)};

    // A tenth of these deliberately name an id that is not resting, so the
    // unknown-order rejection path is exercised rather than assumed.
    if (live_ids.empty() || uniform(0, 99) < 10) {
      command.id = OrderId{next_id_ + 100000U};
      return command;
    }

    command.id = OrderId{
        live_ids[static_cast<std::size_t>(uniform(0, static_cast<int>(live_ids.size()) - 1))]};
    return command;
  }

  GeneratorConfig config_;
  std::mt19937_64 rng_;
  std::uint64_t next_id_ = 1;
  std::uint64_t timestamp_ = 1;
};

// The live id list drives cancel and modify targeting. It is rebuilt from the
// reference rather than from the engine on purpose: if the engine were the source
// of truth here, a bug in the engine could steer the generator away from the very
// commands that would expose it.
void collect_live_ids(const DiffReference& reference, std::vector<std::uint64_t>& out) {
  out.clear();
  for (const Side side : {Side::buy, Side::sell}) {
    for (const DiffReference::LevelView& level : reference.levels(side)) {
      for (const std::uint64_t id : reference.ids_at(side, level.price)) {
        out.push_back(id);
      }
    }
  }
}

// -----------------------------------------------------------------------------
// Shrinking.
//
// A failing sequence of a million commands is not a bug report. Removing commands
// while the failure survives turns it into one, usually of a handful, which is
// small enough to read and to commit.
//
// Repeated delta debugging passes: try dropping a contiguous chunk, keep the drop
// if the failure survives, halve the chunk when a pass makes no progress. Commands
// are not independent, since a cancel refers to an earlier add, so many candidate
// drops change the failure rather than preserving it. Those are rejected by
// re-running, which is why this is worth doing automatically and not by hand.
// -----------------------------------------------------------------------------

[[nodiscard]] std::vector<Command> shrink(const std::vector<Command>& failing) {
  std::vector<Command> best = failing;

  for (std::size_t chunk = best.size() / 2; chunk > 0; chunk /= 2) {
    bool progress = true;
    while (progress) {
      progress = false;
      for (std::size_t start = 0; start + chunk <= best.size();) {
        std::vector<Command> candidate;
        candidate.reserve(best.size() - chunk);
        candidate.insert(
            candidate.end(), best.begin(), best.begin() + static_cast<std::ptrdiff_t>(start));
        candidate.insert(
            candidate.end(), best.begin() + static_cast<std::ptrdiff_t>(start + chunk), best.end());

        if (!candidate.empty() && replay(candidate).mismatch.has_value()) {
          best = candidate;
          progress = true;
        } else {
          start += chunk;
        }
      }
    }
  }

  return best;
}

[[nodiscard]] std::filesystem::path regressions_directory() {
  return std::filesystem::path(OB_TEST_SOURCE_DIR) / "regressions";
}

void record_regression(const std::vector<Command>& minimal, std::uint64_t seed) {
  const std::filesystem::path directory = regressions_directory();
  std::error_code code;
  std::filesystem::create_directories(directory, code);

  const std::filesystem::path file = directory / ("mismatch_seed_" + std::to_string(seed) + ".txt");
  std::ofstream out(file);
  if (out) {
    out << encode(minimal);
  }

  std::cerr << "minimal reproducer written to " << file.string() << "\n" << encode(minimal);
}

// -----------------------------------------------------------------------------
// The tests.
// -----------------------------------------------------------------------------

[[nodiscard]] std::size_t command_budget() {
  // Set by CI: a million per push, ten million on the nightly schedule. The
  // default keeps a local run to a few seconds.
  const char* const configured = std::getenv("OB_DIFF_COMMANDS");
  if (configured == nullptr) {
    return 200000;
  }

  const long long parsed = std::strtoll(configured, nullptr, 10);
  return parsed > 0 ? static_cast<std::size_t>(parsed) : 200000;
}

void run_seed(std::uint64_t seed, std::size_t budget) {
  GeneratorConfig config;
  config.seed = seed;
  CommandGenerator generator(config);

  DiffEngine engine(diff_engine_config());
  DiffReference reference(diff_price_config());
  DiffRing ring;

  std::vector<Command> history;
  history.reserve(budget);

  std::vector<std::uint64_t> live_ids;
  std::vector<ExecutionEvent> fast_events;
  std::vector<ExecutionEvent> slow_events;

  for (std::size_t issued = 0; issued < budget; ++issued) {
    const Command command = generator.next(live_ids);
    history.push_back(command);

    if (const std::optional<Mismatch> mismatch =
            step(command, engine, reference, ring, fast_events, slow_events)) {
      const std::vector<Command> minimal = shrink(history);
      record_regression(minimal, seed);
      FAIL() << "seed " << seed << " diverged at command " << issued << ": "
             << mismatch->description << "\nshrunk from " << history.size() << " commands to "
             << minimal.size();
    }

    collect_live_ids(reference, live_ids);
  }
}

TEST(Differential, MatchesTheReferenceBookAcrossRandomisedCommands) {
  const std::size_t budget = command_budget();

  // Several seeds rather than one long run. Independent sequences explore
  // different regions of the state space, and a fixed seed set means a failure is
  // reproducible from the seed alone.
  constexpr std::array<std::uint64_t, 5> SEEDS{1, 7, 42, 1337, 90210};
  constexpr std::size_t SEED_COUNT = SEEDS.size();

  for (const std::uint64_t seed : SEEDS) {
    run_seed(seed, budget / SEED_COUNT);
  }

  std::cout << "differential: " << budget << " commands across " << SEED_COUNT << " seeds\n";
}

// Deliberately adversarial: one participant, so nearly every match is a self
// trade, and a price span of two so every order collides.
TEST(Differential, SurvivesAHighSelfTradeRate) {
  GeneratorConfig config;
  config.seed = 555;
  config.participants = 1;
  config.price_span = 2;
  CommandGenerator generator(config);

  DiffEngine engine(diff_engine_config());
  DiffReference reference(diff_price_config());
  DiffRing ring;

  std::vector<std::uint64_t> live_ids;
  std::vector<ExecutionEvent> fast_events;
  std::vector<ExecutionEvent> slow_events;

  for (std::size_t i = 0; i < 20000; ++i) {
    const Command command = generator.next(live_ids);
    if (const std::optional<Mismatch> mismatch =
            step(command, engine, reference, ring, fast_events, slow_events)) {
      FAIL() << "diverged at command " << i << ": " << mismatch->description;
    }
    collect_live_ids(reference, live_ids);
  }
}

// A deep book across a wide price span, which is the opposite regime to the test
// above: mostly passive orders, many occupied levels, and pressure on the
// occupancy bitmap rather than on the matcher.
//
// The first version of this test placed orders uniformly across the span and
// asserted the book would be deep. It was not, and the assertion caught it: with
// uniform placement a buy near the top of the span crosses every ask beneath it,
// so a wide span produces a repeatedly swept book rather than a deep one, and only
// two levels survived. Building depth needs orders placed passively, which is what
// passive_bias does.
TEST(Differential, SurvivesAWideSparseBook) {
  GeneratorConfig config;
  config.seed = 31415;
  config.price_span = 900;
  config.participants = 8;
  config.passive_bias = true;
  CommandGenerator generator(config);

  DiffEngine engine(diff_engine_config());
  DiffReference reference(diff_price_config());
  DiffRing ring;

  std::vector<std::uint64_t> live_ids;
  std::vector<ExecutionEvent> fast_events;
  std::vector<ExecutionEvent> slow_events;

  for (std::size_t i = 0; i < 30000; ++i) {
    const Command command = generator.next(live_ids);
    if (const std::optional<Mismatch> mismatch =
            step(command, engine, reference, ring, fast_events, slow_events)) {
      FAIL() << "diverged at command " << i << ": " << mismatch->description;
    }
    collect_live_ids(reference, live_ids);
  }

  EXPECT_GT(engine.book().occupied_level_count(Side::buy), 50U)
      << "the wide span should have produced a genuinely sparse book";
}

// Prices deliberately spread wider than the band, so a large fraction of the book
// lives in cold storage at any moment.
//
// This configuration exists because a code review found a bug the entire
// existing differential suite was blind to. Every other configuration uses a price
// span of 2 to 900 ticks inside a 4096 level band, so no command sequence ever
// combined fill-or-kill with cold levels. The liquidity precheck walked only the
// band bitmap while the matcher walked cold levels too, so a fill-or-kill the
// matcher would have filled in full was rejected as insufficient. The reference book
// has no band concept and counts everything, so this test fails against the old
// engine and passes against the fixed one.
//
// The cold cap is raised well above the number of distinct prices this span can
// produce. That is not papering over anything: the reference book is unbounded by
// design, so a band_overflow rejection would be a capacity divergence rather than a
// semantic one, and it would mask the behaviour under test.
TEST(Differential, MatchesWhenLiquiditySpansTheBandEdgeIntoColdStorage) {
  GeneratorConfig config;
  config.seed = 606060;
  config.price_span = 3000;  // band covers 4096 ticks, so this overflows it
  config.participants = 3;
  CommandGenerator generator(config);

  DiffEngine::Config engine_config;
  engine_config.price = diff_price_config();
  engine_config.arena_capacity = 1U << 15;
  engine_config.max_cold_levels_per_side = 8192;
  engine_config.initial_center = Ticks{0};

  DiffEngine engine(engine_config);
  DiffReference reference(diff_price_config());
  DiffRing ring;

  std::vector<std::uint64_t> live_ids;
  std::vector<ExecutionEvent> fast_events;
  std::vector<ExecutionEvent> slow_events;

  for (std::size_t i = 0; i < 60000; ++i) {
    const Command command = generator.next(live_ids);

    ring.clear();
    fast_events.clear();
    slow_events.clear();

    engine.submit(command, ring);
    reference.submit(command, slow_events);

    ExecutionEvent event;
    while (ring.pop(event)) {
      fast_events.push_back(event);
    }

    ASSERT_FALSE(ring.overflowed()) << "command " << i;
    if (const std::optional<Mismatch> events = compare_events(fast_events, slow_events)) {
      FAIL() << "command " << i << ": " << events->description;
    }
    if (const std::optional<Mismatch> state = compare_state(engine, reference)) {
      FAIL() << "command " << i << ": " << state->description;
    }

    collect_live_ids(reference, live_ids);
  }

  // Without these the test could pass by never entering the regime it exists to
  // cover, which is the failure mode that let the original bug survive.
  EXPECT_GT(engine.book().cold_level_count(), 0U)
      << "no cold levels, so this test did not exercise the path it was written for";
  EXPECT_GT(engine.book().rebase_count(), 0U) << "the wide span should have forced rebasing";
}

// The cancel-oldest policy needs exercising too, or the template parameter is
// only ever proven for its default instantiation.
TEST(Differential, MatchesUnderCancelOldestPolicy) {
  using OldestEngine = Engine<CancelOldest, 4096>;
  using OldestReference = testing::ReferenceBook<CancelOldest>;

  GeneratorConfig config;
  config.seed = 24601;
  config.participants = 2;
  CommandGenerator generator(config);

  OldestEngine::Config engine_config;
  engine_config.price = diff_price_config();
  engine_config.arena_capacity = 8192;
  engine_config.max_cold_levels_per_side = 64;
  engine_config.initial_center = Ticks{0};

  OldestEngine engine(engine_config);
  OldestReference reference(diff_price_config());
  DiffRing ring;

  std::vector<std::uint64_t> live_ids;
  std::vector<ExecutionEvent> fast_events;
  std::vector<ExecutionEvent> slow_events;

  for (std::size_t i = 0; i < 50000; ++i) {
    const Command command = generator.next(live_ids);

    ring.clear();
    fast_events.clear();
    slow_events.clear();

    engine.submit(command, ring);
    reference.submit(command, slow_events);

    ExecutionEvent event;
    while (ring.pop(event)) {
      fast_events.push_back(event);
    }

    ASSERT_FALSE(ring.overflowed()) << "command " << i;
    if (const std::optional<Mismatch> events = compare_events(fast_events, slow_events)) {
      FAIL() << "command " << i << ": " << events->description;
    }

    ASSERT_EQ(engine.book().best_bid(), reference.best_bid()) << "command " << i;
    ASSERT_EQ(engine.book().best_ask(), reference.best_ask()) << "command " << i;
    ASSERT_EQ(engine.book().pool().live_count(), reference.live_count()) << "command " << i;

    live_ids.clear();
    for (const Side side : {Side::buy, Side::sell}) {
      for (const OldestReference::LevelView& level : reference.levels(side)) {
        for (const std::uint64_t id : reference.ids_at(side, level.price)) {
          live_ids.push_back(id);
        }
      }
    }
  }
}

// Every reproducer ever committed replays on every push, whatever the randomised
// budget is set to. This is what makes the reduced push budget safe: a bug found
// once at any budget is pinned by a deterministic test from then on.
TEST(Differential, ReplaysEveryCommittedRegression) {
  const std::filesystem::path directory = regressions_directory();
  if (!std::filesystem::exists(directory)) {
    GTEST_SKIP() << "no regressions directory, which means no mismatch has ever been found";
  }

  std::size_t replayed = 0;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != ".txt") {
      continue;
    }

    std::ifstream input(entry.path());
    ASSERT_TRUE(input) << "cannot open " << entry.path().string();

    const std::vector<Command> commands = decode(input);
    if (commands.empty()) {
      continue;
    }

    if (const ReplayResult result = replay(commands); result.mismatch.has_value()) {
      ADD_FAILURE() << entry.path().filename().string() << " still diverges at command "
                    << result.failing_index << ": " << result.mismatch->description;
    }
    ++replayed;
  }

  std::cout << "replayed " << replayed << " committed regressions\n";
}

// The reproducer format has to survive a round trip or a committed regression is
// not the sequence that failed.
TEST(Differential, ReproducerFormatRoundTrips) {
  GeneratorConfig config;
  config.seed = 8675309;
  CommandGenerator generator(config);

  std::vector<Command> original;
  original.reserve(200);
  std::vector<std::uint64_t> live_ids;
  for (std::size_t i = 0; i < 200; ++i) {
    original.push_back(generator.next(live_ids));
  }

  std::istringstream encoded(encode(original));
  const std::vector<Command> decoded = decode(encoded);

  ASSERT_EQ(decoded.size(), original.size());
  for (std::size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(decoded[i].type, original[i].type) << "command " << i;
    EXPECT_EQ(decoded[i].order_type, original[i].order_type) << "command " << i;
    EXPECT_EQ(decoded[i].side, original[i].side) << "command " << i;
    EXPECT_EQ(decoded[i].id, original[i].id) << "command " << i;
    EXPECT_EQ(decoded[i].price, original[i].price) << "command " << i;
    EXPECT_EQ(decoded[i].quantity, original[i].quantity) << "command " << i;
    EXPECT_EQ(decoded[i].timestamp, original[i].timestamp) << "command " << i;
    EXPECT_EQ(decoded[i].party, original[i].party) << "command " << i;
  }
}

// At the representable-price endpoints the engine and the reference book
// must agree on the rejection reason. The fringe of interest is the one-to-
// tick_size-1 raw units just past an endpoint, where the truncating
// representable() quotient still says true while the inclusive bounds say out of
// range. The two implementations must both call that band_overflow, and an in-range
// off-grid price off_tick. Full events are compared, not helper booleans.
void expect_fringe_equivalent(const PriceConfig& price, const char* label) {
  const std::int64_t lo = price.min_representable_raw();
  const std::int64_t hi = price.max_representable_raw();
  const std::int64_t ts = price.tick_size;
  constexpr std::int64_t I64_MIN = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t I64_MAX = std::numeric_limits<std::int64_t>::max();

  std::vector<std::int64_t> raws;
  for (const std::int64_t off : {std::int64_t{0}, std::int64_t{1}, ts - 1, ts}) {
    if (hi >= I64_MIN + off) {
      raws.push_back(hi - off);
    }
    if (hi <= I64_MAX - off) {
      raws.push_back(hi + off);
    }
    if (lo >= I64_MIN + off) {
      raws.push_back(lo - off);
    }
    if (lo <= I64_MAX - off) {
      raws.push_back(lo + off);
    }
  }

  std::uint64_t id = 1;
  for (const std::int64_t raw : raws) {
    const bool in_range = raw >= lo && raw <= hi;
    // Skip in-range, on-grid prices: those are accepted and placed, which the main
    // differential already covers. This test is about rejection classification.
    if (in_range && price.on_tick_boundary(Price{raw})) {
      continue;
    }

    DiffEngine::Config config;
    config.price = price;
    config.arena_capacity = 1024;
    config.max_cold_levels_per_side = 64;
    config.initial_center = Ticks{0};

    DiffEngine engine(config);
    DiffReference reference(price);
    DiffRing ring;
    std::vector<ExecutionEvent> fast;
    std::vector<ExecutionEvent> slow;

    Command command;
    command.type = CommandType::add;
    command.order_type = OrderType::limit;
    command.side = Side::buy;
    command.id = OrderId{id++};
    command.price = Price{raw};
    command.quantity = Quantity{10};
    command.timestamp = Timestamp{id};

    engine.submit(command, ring);
    reference.submit(command, slow);
    ExecutionEvent event;
    while (ring.pop(event)) {
      fast.push_back(event);
    }

    const std::optional<Mismatch> mismatch = compare_events(fast, slow);
    ASSERT_FALSE(mismatch.has_value())
        << label << " raw=" << raw << ": " << (mismatch ? mismatch->description : std::string());
  }
}

TEST(DifferentialBoundary, EngineAndReferenceClassifyFringesIdentically) {
  expect_fringe_equivalent(
      PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = 1000000},
      "positive base, tick 100");
  expect_fringe_equivalent(PriceConfig{.tick_size = 1, .price_scale = 10000, .base_price = 0},
                           "zero base, unit tick");
  expect_fringe_equivalent(
      PriceConfig{.tick_size = 100, .price_scale = 10000, .base_price = -1000000},
      "negative base, tick 100");
  expect_fringe_equivalent(PriceConfig{.tick_size = std::numeric_limits<std::int64_t>::max() / 4,
                                       .price_scale = 10000,
                                       .base_price = 0},
                           "int64-binding tick");
}

}  // namespace
}  // namespace ob
