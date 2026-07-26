#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "itch/mapped_file.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"
#include "itch/replay.hpp"
#include "itch/synthetic.hpp"
#include "ob/book.hpp"

namespace ob::itch {
namespace {

constexpr std::size_t TEST_BAND = 4096;
using TestBook = ob::Book<TEST_BAND>;
using TestDriver = ReplayDriver<TEST_BAND>;

constexpr std::int64_t TEST_BASE_PRICE = 1000000;
constexpr std::int64_t TEST_TICK_SIZE = 100;

TestBook::Config make_book_config() {
  TestBook::Config config;
  config.price = ob::PriceConfig{
      .tick_size = TEST_TICK_SIZE, .price_scale = PRICE_SCALE, .base_price = TEST_BASE_PRICE};
  config.arena_capacity = 1U << 16;
  config.max_cold_levels_per_side = 256;
  config.initial_center = ob::Ticks{0};
  return config;
}

SyntheticConfig make_generator_config(std::size_t messages, std::uint64_t seed) {
  SyntheticConfig config;
  config.symbol = "TEST";
  config.seed = seed;
  config.message_count = messages;
  config.initial_price = TEST_BASE_PRICE;
  config.tick_size = TEST_TICK_SIZE;
  return config;
}

// ---------------------------------------------------------------------------
// Byte level decoding.
//
// These are the assertions that matter most for a wire format. Every one of them
// would pass by accident on a little endian machine if the code reinterpret_cast a
// struct over the buffer, and every one of them would then be wrong.
// ---------------------------------------------------------------------------

TEST(ItchLoaders, DecodeBigEndianRegardlessOfHostOrder) {
  const std::array<std::byte, 8> bytes{std::byte{0x01},
                                       std::byte{0x02},
                                       std::byte{0x03},
                                       std::byte{0x04},
                                       std::byte{0x05},
                                       std::byte{0x06},
                                       std::byte{0x07},
                                       std::byte{0x08}};

  EXPECT_EQ(load_be16(bytes.data()), 0x0102U);
  EXPECT_EQ(load_be32(bytes.data()), 0x01020304U);
  EXPECT_EQ(load_be48(bytes.data()), 0x010203040506ULL);
  EXPECT_EQ(load_be64(bytes.data()), 0x0102030405060708ULL);
}

TEST(ItchLoaders, MaximumValuesRoundTrip) {
  std::array<std::byte, 8> bytes{};

  store_be16(bytes.data(), 0xFFFFU);
  EXPECT_EQ(load_be16(bytes.data()), 0xFFFFU);

  store_be32(bytes.data(), 0xFFFFFFFFU);
  EXPECT_EQ(load_be32(bytes.data()), 0xFFFFFFFFU);

  // The forty eight bit timestamp is the awkward one: it has no native type, so
  // the top two bytes of the assembled value must stay clear.
  store_be48(bytes.data(), 0xFFFFFFFFFFFFULL);
  EXPECT_EQ(load_be48(bytes.data()), 0xFFFFFFFFFFFFULL);

  store_be64(bytes.data(), 0xFFFFFFFFFFFFFFFFULL);
  EXPECT_EQ(load_be64(bytes.data()), 0xFFFFFFFFFFFFFFFFULL);
}

TEST(ItchMessages, LengthTableMatchesTheSpecification) {
  // Spot checked against the TotalView-ITCH 5.0 specification. A wrong entry here
  // desynchronises any stream without a length prefix and is caught by the
  // parser's cross check on any stream with one.
  EXPECT_EQ(message_length('S'), 12U);
  EXPECT_EQ(message_length('R'), 39U);
  EXPECT_EQ(message_length('A'), 36U);
  EXPECT_EQ(message_length('F'), 40U);
  EXPECT_EQ(message_length('E'), 31U);
  EXPECT_EQ(message_length('C'), 36U);
  EXPECT_EQ(message_length('X'), 23U);
  EXPECT_EQ(message_length('D'), 19U);
  EXPECT_EQ(message_length('U'), 35U);
  EXPECT_EQ(message_length('P'), 44U);
  EXPECT_EQ(message_length('Q'), 40U);
  EXPECT_EQ(message_length('B'), 19U);

  // Types the driver skips but must still measure correctly.
  EXPECT_EQ(message_length('H'), 25U);
  EXPECT_EQ(message_length('Y'), 20U);
  EXPECT_EQ(message_length('I'), 50U);
  EXPECT_EQ(message_length('h'), 21U);  // lower case in the specification

  EXPECT_EQ(message_length('Z'), 0U);
  EXPECT_FALSE(is_known_type('Z'));
  EXPECT_TRUE(is_known_type('A'));
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

TEST(ItchParser, WalksAGeneratedStreamCompletely) {
  SyntheticGenerator generator(make_generator_config(5000, 11));
  const std::vector<std::byte> data = generator.generate();
  ASSERT_FALSE(data.empty());

  std::size_t seen = 0;
  std::map<char, std::size_t> by_type;
  const ParseResult result = for_each_message(data, [&](const MessageView& message) {
    ++seen;
    ++by_type[message.raw_type()];
    EXPECT_EQ(message.size(), message_length(message.raw_type()));
  });

  EXPECT_EQ(result.status, ParseStatus::ok);
  EXPECT_EQ(result.messages, generator.summary().messages);
  EXPECT_EQ(result.bytes_consumed, data.size());
  EXPECT_EQ(seen, generator.summary().messages);
  EXPECT_EQ(result.unknown_types, 0U);

  // Report the distribution, because a benchmark on this file is only meaningful
  // alongside the mix it replayed.
  std::cout << "generated message distribution over " << seen << " messages:\n";
  for (const std::pair<const char, std::size_t>& entry : by_type) {
    std::cout << "  " << entry.first << ": " << entry.second << '\n';
  }
}

TEST(ItchParser, StopsCleanlyOnATruncatedStream) {
  SyntheticGenerator generator(make_generator_config(200, 5));
  std::vector<std::byte> data = generator.generate();
  ASSERT_GT(data.size(), 40U);

  // Cut the file mid message. A parser that trusted its cursor would read past
  // the end; this one has to notice and say so.
  data.resize(data.size() - 7);

  std::size_t seen = 0;
  const ParseResult result = for_each_message(data, [&seen](const MessageView&) { ++seen; });

  EXPECT_EQ(result.status, ParseStatus::truncated_message);
  EXPECT_EQ(result.messages, seen);
  EXPECT_GT(seen, 0U) << "everything before the cut should still have parsed";
}

TEST(ItchParser, RejectsAnImplausibleLengthPrefix) {
  std::vector<std::byte> data(8, std::byte{0});
  store_be16(data.data(), 9999);  // longer than any message this format defines

  const ParseResult result = for_each_message(data, [](const MessageView&) {});
  EXPECT_EQ(result.status, ParseStatus::implausible_length);
  EXPECT_EQ(result.messages, 0U);
}

TEST(ItchParser, RejectsAPrefixThatDisagreesWithTheTypesKnownLength) {
  // A well formed add order, but with the prefix claiming a delete's length.
  //
  // Constructed at its final size rather than default constructed and resized.
  // That is not a style preference: GCC 16 at -O3 could not establish the bound
  // through the resize and rejected the inlined vector fill with a spurious
  // -Warray-bounds, which -Werror turned into a build failure. Giving the size to
  // the constructor makes it a constant the optimiser can see.
  std::vector<std::byte> data(2 + add_order::LENGTH, std::byte{0});
  store_be16(data.data(), static_cast<std::uint16_t>(order_delete::LENGTH));
  data[2] = static_cast<std::byte>('A');

  const ParseResult result = for_each_message(data, [](const MessageView&) {});
  EXPECT_EQ(result.status, ParseStatus::length_disagreement);
}

// An unknown type must advance the cursor by its declared length rather than
// desynchronise the stream. This is the property the framing design exists for.
TEST(ItchParser, SkipsAnUnknownTypeWithoutLosingTheStream) {
  constexpr std::size_t UNKNOWN_LENGTH = 17;
  constexpr std::size_t TOTAL_BYTES =
      (2 + order_delete::LENGTH) + (2 + UNKNOWN_LENGTH) + (2 + order_delete::LENGTH);

  // Sized once and never grown. Appending message by message is the natural way to
  // write this, and it is what the first version did, but GCC 16 at -O3 could not
  // model the vector's reallocation through the loop and rejected the inlined fill
  // with a spurious -Warray-bounds that -Werror turned into a build failure.
  // Computing the total up front sidesteps the growth entirely.
  std::vector<std::byte> data(TOTAL_BYTES, std::byte{0});

  std::size_t cursor = 0;
  const auto write = [&data, &cursor](char type, std::size_t length) {
    store_be16(data.data() + cursor, static_cast<std::uint16_t>(length));
    data[cursor + 2] = static_cast<std::byte>(static_cast<unsigned char>(type));
    cursor += 2 + length;
  };

  write('D', order_delete::LENGTH);
  write('~', UNKNOWN_LENGTH);  // not a type this build knows
  write('D', order_delete::LENGTH);

  std::vector<char> types;
  const ParseResult result = for_each_message(
      data, [&types](const MessageView& message) { types.push_back(message.raw_type()); });

  EXPECT_EQ(result.status, ParseStatus::ok);
  EXPECT_EQ(result.messages, 3U);
  EXPECT_EQ(result.unknown_types, 1U);
  EXPECT_EQ(types, (std::vector<char>{'D', '~', 'D'}));
  EXPECT_EQ(result.bytes_consumed, data.size());
}

TEST(ItchParser, HandlesAnEmptyStream) {
  const std::vector<std::byte> data;
  const ParseResult result = for_each_message(data, [](const MessageView&) {});
  EXPECT_EQ(result.status, ParseStatus::ok);
  EXPECT_EQ(result.messages, 0U);
}

// ---------------------------------------------------------------------------
// Replay semantics
// ---------------------------------------------------------------------------

// Builds a stream by hand so each message's effect can be asserted in isolation.
// The generator is good for volume; it is useless for pinning down a single rule.
class HandBuiltStream {
 public:
  void add(std::uint64_t reference, bool is_buy, std::uint32_t shares, std::int64_t price) {
    begin('A', add_order::LENGTH);
    u64(reference);
    u8(is_buy ? BUY_INDICATOR : SELL_INDICATOR);
    u32(shares);
    symbol("TEST");
    u32(static_cast<std::uint32_t>(price));
  }

  void execute(std::uint64_t reference, std::uint32_t shares) {
    begin('E', order_executed::LENGTH);
    u64(reference);
    u32(shares);
    u64(1);
  }

  void cancel(std::uint64_t reference, std::uint32_t shares) {
    begin('X', order_cancel::LENGTH);
    u64(reference);
    u32(shares);
  }

  void remove(std::uint64_t reference) {
    begin('D', order_delete::LENGTH);
    u64(reference);
  }

  void replace(std::uint64_t original,
               std::uint64_t replacement,
               std::uint32_t shares,
               std::int64_t price) {
    begin('U', order_replace::LENGTH);
    u64(original);
    u64(replacement);
    u32(shares);
    u32(static_cast<std::uint32_t>(price));
  }

  void cross_trade(std::uint64_t shares, std::int64_t price) {
    begin('Q', cross_trade::LENGTH);
    u64(shares);
    symbol("TEST");
    u32(static_cast<std::uint32_t>(price));
    u64(2);
    u8('O');
  }

  void broken_trade() {
    begin('B', broken_trade::LENGTH);
    u64(2);
  }

  void non_cross_trade(std::uint32_t shares, std::int64_t price) {
    begin('P', trade::LENGTH);
    u64(0);
    u8(BUY_INDICATOR);
    u32(shares);
    symbol("TEST");
    u32(static_cast<std::uint32_t>(price));
    u64(3);
  }

  void directory(const std::string& name, std::uint16_t locate) {
    begin_with_locate('R', stock_directory::LENGTH, locate);
    symbol(name);
    // Pad out the remaining directory fields. Their values do not matter here;
    // only the message's total length does, because that is what keeps the stream
    // framed.
    for (std::size_t i = 0; i < stock_directory::LENGTH - HEADER_LENGTH - SYMBOL_LENGTH; ++i) {
      u8('N');
    }
  }

  [[nodiscard]] std::span<const std::byte> bytes() const { return data_; }

 private:
  void begin(char type, std::size_t length) { begin_with_locate(type, length, 1); }

  void begin_with_locate(char type, std::size_t length, std::uint16_t locate) {
    const std::size_t start = data_.size();

    // One insert of a compile time constant count, rather than two successive
    // resizes. GCC 16 at -O3 could not track the vector's extent through the pair
    // and rejected the inlined fill with a spurious -Warray-bounds, which -Werror
    // turned into a build failure. Growing once is also simply clearer.
    data_.insert(data_.end(), 2 + HEADER_LENGTH, std::byte{0});

    store_be16(data_.data() + start, static_cast<std::uint16_t>(length));
    std::byte* const header = data_.data() + start + 2;
    header[OFFSET_MESSAGE_TYPE] = static_cast<std::byte>(static_cast<unsigned char>(type));
    store_be16(header + OFFSET_STOCK_LOCATE, locate);
    store_be16(header + OFFSET_TRACKING_NUMBER, 0);
    store_be48(header + OFFSET_TIMESTAMP, ++clock_);
  }

  void u8(char value) {
    data_.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
  }

  void u32(std::uint32_t value) {
    std::array<std::byte, 4> scratch{};
    store_be32(scratch.data(), value);
    for (const std::byte byte : scratch) {
      data_.push_back(byte);
    }
  }

  void u64(std::uint64_t value) {
    std::array<std::byte, 8> scratch{};
    store_be64(scratch.data(), value);
    for (const std::byte byte : scratch) {
      data_.push_back(byte);
    }
  }

  void symbol(const std::string& name) {
    for (std::size_t i = 0; i < SYMBOL_LENGTH; ++i) {
      u8(i < name.size() ? name[i] : ' ');
    }
  }

  std::vector<std::byte> data_;
  std::uint64_t clock_ = 34200000000000;
};

std::int64_t price_at(std::int32_t tick) {
  return TEST_BASE_PRICE + (static_cast<std::int64_t>(tick) * TEST_TICK_SIZE);
}

TEST(ItchReplay, AddsBuildTheBook) {
  HandBuiltStream stream;
  stream.directory("TEST", 1);
  stream.add(101, true, 300, price_at(-2));
  stream.add(102, false, 200, price_at(3));

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(stream.bytes(), driver).status, ParseStatus::ok);

  EXPECT_TRUE(driver.symbol_resolved());
  EXPECT_EQ(driver.resolved_locate(), 1U);
  EXPECT_EQ(book->best_bid(), ob::Ticks{-2});
  EXPECT_EQ(book->best_ask(), ob::Ticks{3});
  EXPECT_EQ(book->total_qty_at(ob::Side::buy, ob::Ticks{-2}), ob::Quantity{300});
  EXPECT_EQ(driver.stats().add_failures, 0U);
}

TEST(ItchReplay, ExecutionReducesAndRemovesAtZero) {
  HandBuiltStream stream;
  stream.directory("TEST", 1);
  stream.add(101, true, 300, price_at(-2));
  stream.execute(101, 100);

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(stream.bytes(), driver).status, ParseStatus::ok);

  // A partial execution leaves the order resting with its queue position intact.
  EXPECT_EQ(book->total_qty_at(ob::Side::buy, ob::Ticks{-2}), ob::Quantity{200});
  EXPECT_EQ(book->pool().live_count(), 1U);

  HandBuiltStream finish;
  finish.directory("TEST", 1);
  finish.add(101, true, 300, price_at(-2));
  finish.execute(101, 300);

  const std::unique_ptr<TestBook> second = std::make_unique<TestBook>(make_book_config());
  TestDriver second_driver(*second, "TEST");
  ASSERT_EQ(replay_buffer(finish.bytes(), second_driver).status, ParseStatus::ok);

  EXPECT_EQ(second->pool().live_count(), 0U);
  EXPECT_FALSE(second->best_bid().has_value());
}

TEST(ItchReplay, CancelReducesWithoutRemovingUnlessItReachesZero) {
  HandBuiltStream stream;
  stream.directory("TEST", 1);
  stream.add(101, false, 500, price_at(4));
  stream.cancel(101, 200);

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(stream.bytes(), driver).status, ParseStatus::ok);

  EXPECT_EQ(book->total_qty_at(ob::Side::sell, ob::Ticks{4}), ob::Quantity{300});
  EXPECT_EQ(book->pool().live_count(), 1U);
}

TEST(ItchReplay, DeleteRemovesTheWholeOrder) {
  HandBuiltStream stream;
  stream.directory("TEST", 1);
  stream.add(101, false, 500, price_at(4));
  stream.remove(101);

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(stream.bytes(), driver).status, ParseStatus::ok);

  EXPECT_EQ(book->pool().live_count(), 0U);
  EXPECT_FALSE(book->best_ask().has_value());
}

// The replay semantics that matter most, and the one most easily got
// wrong: a replace is a delete plus an add under a new reference, and the new
// order goes to the back of its queue.
TEST(ItchReplay, ReplaceLosesQueuePriority) {
  HandBuiltStream stream;
  stream.directory("TEST", 1);
  stream.add(101, true, 100, price_at(-1));
  stream.add(102, true, 100, price_at(-1));
  stream.add(103, true, 100, price_at(-1));
  // Replace the order at the front of the queue, keeping the same price.
  stream.replace(101, 201, 100, price_at(-1));

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(stream.bytes(), driver).status, ParseStatus::ok);

  std::vector<std::uint64_t> queue;
  for (ob::ArenaIndex index = book->first_order_at(ob::Side::buy, ob::Ticks{-1});
       index != ob::INVALID_INDEX;
       index = book->pool()[index].next) {
    queue.push_back(book->pool()[index].id.raw());
  }

  // 201 is at the back, not where 101 was. Implementing a replace as a modify
  // would have left it at the front and flattered every queue position estimate
  // built on top of this.
  EXPECT_EQ(queue, (std::vector<std::uint64_t>{102, 103, 201}));
  EXPECT_EQ(book->find_order(ob::OrderId{101}), ob::INVALID_INDEX);
  EXPECT_EQ(driver.stats().replaces, 1U);
}

TEST(ItchReplay, ReplaceInfersTheSideFromTheOriginalOrder) {
  HandBuiltStream stream;
  stream.directory("TEST", 1);
  stream.add(101, false, 100, price_at(5));
  stream.replace(101, 201, 150, price_at(6));

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(stream.bytes(), driver).status, ParseStatus::ok);

  // The replace message carries no side field, so it has to come from the order
  // being replaced. Getting this wrong would silently move liquidity to the other
  // side of the book.
  EXPECT_EQ(book->best_ask(), ob::Ticks{6});
  EXPECT_FALSE(book->best_bid().has_value());
  EXPECT_EQ(book->total_qty_at(ob::Side::sell, ob::Ticks{6}), ob::Quantity{150});
}

TEST(ItchReplay, CrossBrokenAndNonCrossTradesDoNotTouchTheBook) {
  HandBuiltStream stream;
  stream.directory("TEST", 1);
  stream.add(101, true, 400, price_at(-3));
  stream.cross_trade(10000, price_at(0));
  stream.broken_trade();
  stream.non_cross_trade(250, price_at(0));

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(stream.bytes(), driver).status, ParseStatus::ok);

  EXPECT_EQ(book->total_qty_at(ob::Side::buy, ob::Ticks{-3}), ob::Quantity{400});
  EXPECT_EQ(book->pool().live_count(), 1U);
  EXPECT_EQ(driver.stats().cross_trades, 1U);
  EXPECT_EQ(driver.stats().broken_trades, 1U);
  EXPECT_EQ(driver.stats().trades, 1U);
  EXPECT_EQ(driver.stats().unknown_order_references, 0U)
      << "a non-cross trade names a non-displayed order and must be ignored, not looked up";
}

TEST(ItchReplay, FiltersByLocateCodeNotBySymbolString) {
  HandBuiltStream stream;
  stream.directory("TEST", 1);
  stream.directory("OTHER", 2);
  stream.add(101, true, 100, price_at(-1));

  // Same message shape, different locate. The execution and delete messages carry
  // no symbol at all, so filtering by locate is the only thing that can work.
  {
    HandBuiltStream other;
    other.directory("TEST", 1);
    other.directory("OTHER", 2);
    other.add(101, true, 100, price_at(-1));

    const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
    TestDriver driver(*book, "OTHER");
    ASSERT_EQ(replay_buffer(other.bytes(), driver).status, ParseStatus::ok);

    EXPECT_TRUE(driver.symbol_resolved());
    EXPECT_EQ(driver.resolved_locate(), 2U);
    // The add belongs to locate 1, so filtering to OTHER must discard it.
    EXPECT_EQ(book->pool().live_count(), 0U);
    EXPECT_EQ(driver.stats().other_symbol, 1U);
  }

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(stream.bytes(), driver).status, ParseStatus::ok);
  EXPECT_EQ(book->pool().live_count(), 1U);
}

TEST(ItchReplay, MessagesBeforeTheDirectoryAreIgnoredRatherThanMisattributed) {
  HandBuiltStream stream;
  stream.add(101, true, 100, price_at(-1));  // no directory yet
  stream.directory("TEST", 1);
  stream.add(102, true, 100, price_at(-1));

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(stream.bytes(), driver).status, ParseStatus::ok);

  EXPECT_EQ(book->pool().live_count(), 1U);
  EXPECT_NE(book->find_order(ob::OrderId{102}), ob::INVALID_INDEX);
  EXPECT_EQ(book->find_order(ob::OrderId{101}), ob::INVALID_INDEX);
}

// ---------------------------------------------------------------------------
// End to end validation against the generator's own expectation
// ---------------------------------------------------------------------------

TEST(ItchReplay, FinalBookMatchesTheGeneratorsIndependentExpectation) {
  SyntheticGenerator generator(make_generator_config(50000, 4242));
  const std::vector<std::byte> data = generator.generate();

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  const ParseResult result = replay_buffer(data, driver);

  ASSERT_EQ(result.status, ParseStatus::ok);
  EXPECT_EQ(result.messages, generator.summary().messages);

  // The generator tracked what it built as it built it. The book is compared
  // against that rather than against the parser's own reading of the file, so the
  // parser cannot agree with itself and call that a pass.
  EXPECT_EQ(book->pool().live_count(), generator.summary().expected_live_orders);
  EXPECT_EQ(driver.stats().add_failures, 0U)
      << "the book was sized wrong for this data, so nothing downstream is meaningful";
  EXPECT_EQ(driver.stats().unknown_order_references, 0U)
      << "the generator only references live orders, so any of these is a replay bug";

  std::uint64_t total_shares = 0;
  for (const ob::Side side : {ob::Side::buy, ob::Side::sell}) {
    std::vector<TestBook::LevelSnapshot> levels;
    book->snapshot_levels(side, levels);
    for (const TestBook::LevelSnapshot& level : levels) {
      total_shares += level.aggregate_qty.raw();
    }
  }
  EXPECT_EQ(total_shares, generator.summary().expected_total_shares);

  std::cout << "replayed " << result.messages << " messages, " << driver.stats().messages_applied
            << " applied, " << driver.stats().other_symbol << " filtered out, "
            << book->pool().live_count() << " orders resting, " << book->rebase_count()
            << " rebases\n";
}

TEST(ItchReplay, SurvivesAHardTrendingMarket) {
  SyntheticConfig config = make_generator_config(60000, 777);

  // Drift, not just volatility. A pure random walk drifts as the square root of
  // the message count, so with a 4096 tick band and a 512 tick rebase margin it
  // would need an impractically long stream to reach an edge by chance. A market
  // that trends one way is what actually forces rebasing, and it is the case I most
  // wanted tested aggressively.
  config.volatility_ticks = 2.0;
  config.drift_ticks_per_message = 0.05;  // about 3000 ticks over this stream
  SyntheticGenerator generator(config);
  const std::vector<std::byte> data = generator.generate();

  const std::unique_ptr<TestBook> book = std::make_unique<TestBook>(make_book_config());
  TestDriver driver(*book, "TEST");
  ASSERT_EQ(replay_buffer(data, driver).status, ParseStatus::ok);

  EXPECT_GT(book->rebase_count(), 0U) << "the trend should have forced rebasing";
  EXPECT_EQ(book->pool().live_count(), generator.summary().expected_live_orders);
  EXPECT_EQ(driver.stats().unknown_order_references, 0U);
}

// ---------------------------------------------------------------------------
// Round trip through a real file, exercising the mapping path
// ---------------------------------------------------------------------------

TEST(ItchMappedFile, RoundTripsThroughDiskByteForByte) {
  SyntheticGenerator generator(make_generator_config(20000, 9001));
  const std::vector<std::byte> original = generator.generate();

  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "ob_itch_roundtrip.itch";

  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out);
    // std::byte and char are both byte types, so viewing one as the other is
    // permitted rather than an aliasing violation, and ostream has no std::byte
    // overload.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    out.write(reinterpret_cast<const char*>(original.data()),
              static_cast<std::streamsize>(original.size()));
    ASSERT_TRUE(out.good());
  }

  MappedFile mapped;
  ASSERT_TRUE(mapped.open(path.string())) << mapped.error();
  ASSERT_EQ(mapped.size(), original.size());

  const std::span<const std::byte> loaded = mapped.bytes();
  for (std::size_t i = 0; i < original.size(); ++i) {
    ASSERT_EQ(loaded[i], original[i]) << "byte " << i << " differs after a disk round trip";
  }

  // And the replayed state has to match too, since a file that compares equal but
  // replays differently would mean the parser is reading something other than what
  // it was handed.
  const std::unique_ptr<TestBook> from_memory = std::make_unique<TestBook>(make_book_config());
  TestDriver memory_driver(*from_memory, "TEST");
  ASSERT_EQ(replay_buffer(original, memory_driver).status, ParseStatus::ok);

  const std::unique_ptr<TestBook> from_disk = std::make_unique<TestBook>(make_book_config());
  TestDriver disk_driver(*from_disk, "TEST");
  ASSERT_EQ(replay_buffer(loaded, disk_driver).status, ParseStatus::ok);

  EXPECT_EQ(from_memory->pool().live_count(), from_disk->pool().live_count());
  EXPECT_EQ(from_memory->best_bid(), from_disk->best_bid());
  EXPECT_EQ(from_memory->best_ask(), from_disk->best_ask());

  mapped.close();
  std::error_code code;
  std::filesystem::remove(path, code);
}

TEST(ItchMappedFile, ReportsAMissingFileRatherThanThrowing) {
  MappedFile mapped;
  EXPECT_FALSE(mapped.open("this_path_does_not_exist_ob_itch.itch"));
  EXPECT_FALSE(mapped.error().empty());
  EXPECT_FALSE(mapped.is_open());
}

TEST(ItchGenerator, IsDeterministicForAGivenSeed) {
  SyntheticGenerator first(make_generator_config(3000, 555));
  SyntheticGenerator second(make_generator_config(3000, 555));
  SyntheticGenerator different(make_generator_config(3000, 556));

  const std::vector<std::byte> a = first.generate();
  const std::vector<std::byte> b = second.generate();
  const std::vector<std::byte> c = different.generate();

  EXPECT_EQ(a, b)
      << "the same seed must produce the same file, or no benchmark on it is comparable";
  EXPECT_NE(a, c) << "a different seed should produce a different file";
}

}  // namespace
}  // namespace ob::itch
