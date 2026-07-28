#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "itch/mapped_file.hpp"
#include "itch/messages.hpp"
#include "itch/parser.hpp"
#include "itch/replay.hpp"
#include "json/provenance.hpp"
#include "json/writer.hpp"
#include "ob/book.hpp"

// Replays an ITCH 5.0 file into a book, filtered to one symbol, and reports what
// happened.
//
// This is the tool that answers the question the synthetic tests cannot: does the
// pipeline read a real NASDAQ capture correctly? Synthetic files prove the parser
// is self-consistent and matches the specification as written. They cannot prove
// the specification and the actual feed agree, and in an exchange protocol those
// two things diverge in small ways that only real bytes reveal.
//
// It also has a survey mode, because the first thing anyone needs from an
// unfamiliar capture is the message type histogram and the list of symbols worth
// replaying.

namespace {

// A band wide enough for a real symbol's intraday range. NASDAQ prices are in
// hundredths of a cent, so at a penny tick a 65536 level band spans 655 dollars,
// which covers any ordinary equity's day.
constexpr std::size_t REPLAY_BAND = ob::DEFAULT_BAND_LEVELS;
using ReplayBook = ob::Book<REPLAY_BAND>;
using Driver = ob::itch::ReplayDriver<REPLAY_BAND>;

void print_usage() {
  std::cout <<
      R"(usage: itch_replay [options] <itch-file>

  --symbol NAME     symbol to replay, required unless --survey
  --survey          report the message type histogram and the busiest symbols
  --top N           how many symbols to list in survey mode (default 20)
  --arena N         order arena capacity (default 262144)
  --cold N          maximum cold levels per side (default 8192)
  --limit N         stop after N messages, 0 for the whole file (default 0)
  --tick N          tick size in scaled units, 100 is a penny (default 100)
  --extract FILE    write only this symbol's messages to FILE, as valid ITCH
  --json FILE       also write the replay result to FILE as JSON
  --json-depth FILE also write a depth ladder snapshot to FILE as JSON
  --depth-levels N  levels per side in the depth ladder (default 20)
  --depth-at N      snapshot the ladder after N messages instead of at the end
  --depth-at-peak   snapshot the ladder at the deepest moment of the replay
  --sha256 HEX      record this hash of the input in the JSON provenance block

--extract is how a multi-gigabyte capture becomes a test fixture small enough to
commit: it keeps the system event and stock directory messages the replay driver
needs, plus every message for the chosen symbol, and drops the rest.

--json exists because the results site reads these numbers, and stdout is not an
interface: this repository rewords its output deliberately and often, so anything
that scraped it would break on an edit that changed no behaviour.

--depth-at-peak exists because a full trading day ends with an empty book. The
session close deletes everything, so an end of replay ladder over a whole day is
correct and shows nothing. Snapshotting where the book was deepest gives a ladder
worth looking at and anchors it to something meaningful rather than to an
arbitrary offset. --depth-at names an explicit message index instead.
)";
}

[[nodiscard]] bool match_option(std::string_view argument,
                                std::string_view name,
                                int& index,
                                int argc,
                                char** argv,
                                std::string& value) {
  if (argument != name) {
    return false;
  }
  if (index + 1 >= argc) {
    std::cerr << "missing value for " << name << '\n';
    std::exit(2);
  }
  ++index;
  value = argv[index];
  return true;
}

// Numeric options are parsed defensively rather than with std::stoull, which
// throws on garbage and terminates the tool with an unhandled exception. A typo in
// a command line should print usage and exit, not abort. This mirrors the helpers
// in tools/strategy_backtest.cpp so the two tools behave the same way.
[[nodiscard]] bool parse_u64(const std::string& text, std::uint64_t& out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  if (end == text.c_str() || end == nullptr || *end != '\0') {
    return false;
  }
  out = static_cast<std::uint64_t>(value);
  return true;
}

[[nodiscard]] bool parse_i64(const std::string& text, std::int64_t& out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const long long value = std::strtoll(text.c_str(), &end, 10);
  if (end == text.c_str() || end == nullptr || *end != '\0') {
    return false;
  }
  out = static_cast<std::int64_t>(value);
  return true;
}

[[noreturn]] void bad_value(std::string_view name, const std::string& value) {
  std::cerr << "invalid value for " << name << ": " << value << '\n';
  std::exit(2);
}

struct SymbolActivity {
  std::string symbol;
  std::uint64_t messages = 0;
};

// Survey: histogram by type, and message counts per locate code resolved back to
// symbols via the stock directory.
void survey(std::span<const std::byte> data, std::size_t top) {
  std::map<char, std::uint64_t> by_type;
  std::map<std::uint16_t, std::string> symbols;
  std::map<std::uint16_t, std::uint64_t> by_locate;

  const ob::itch::ParseResult result =
      ob::itch::for_each_message(data, [&](const ob::itch::MessageView& message) {
        ++by_type[message.raw_type()];

        if (message.raw_type() == static_cast<char>(ob::itch::MessageType::stock_directory)) {
          symbols[message.stock_locate()] =
              std::string(message.symbol_at(ob::itch::stock_directory::STOCK));
          return;
        }
        ++by_locate[message.stock_locate()];
      });

  std::cout << "messages parsed      " << result.messages << '\n'
            << "bytes consumed       " << result.bytes_consumed << " of " << data.size() << '\n'
            << "parse status         " << static_cast<int>(result.status) << '\n'
            << "unknown types        " << result.unknown_types << '\n'
            << "symbols in directory " << symbols.size() << "\n\n";

  std::cout << "message type histogram\n";
  for (const std::pair<const char, std::uint64_t>& entry : by_type) {
    std::cout << "  " << entry.first << "  " << std::setw(12) << entry.second << '\n';
  }

  std::vector<SymbolActivity> ranked;
  ranked.reserve(by_locate.size());
  for (const std::pair<const std::uint16_t, std::uint64_t>& entry : by_locate) {
    const auto named = symbols.find(entry.first);
    ranked.push_back(SymbolActivity{
        .symbol = named == symbols.end() ? "?locate=" + std::to_string(entry.first) : named->second,
        .messages = entry.second});
  }
  std::ranges::sort(ranked, [](const SymbolActivity& a, const SymbolActivity& b) {
    return a.messages > b.messages;
  });

  std::cout << "\nbusiest symbols\n";
  for (std::size_t i = 0; i < ranked.size() && i < top; ++i) {
    std::cout << "  " << std::setw(10) << std::left << ranked[i].symbol << std::right
              << std::setw(12) << ranked[i].messages << '\n';
  }
}

// Writes a length prefixed copy of the input containing only the messages a
// single symbol replay needs. Structure is preserved exactly, so the output is a
// valid ITCH 5.0 file readable by the same parser with no special casing.
void extract(std::span<const std::byte> data,
             const std::string& symbol,
             const std::string& path,
             std::size_t limit) {
  std::uint16_t locate = 0;
  bool resolved = false;

  std::vector<std::byte> out;
  std::uint64_t kept = 0;

  const auto keep = [&out](const ob::itch::MessageView& message) {
    std::array<std::byte, 2> prefix{};
    ob::itch::store_be16(prefix.data(), static_cast<std::uint16_t>(message.size()));
    out.push_back(prefix[0]);
    out.push_back(prefix[1]);
    out.insert(out.end(), message.bytes().begin(), message.bytes().end());
  };

  std::size_t seen = 0;
  static_cast<void>(ob::itch::for_each_message(data, [&](const ob::itch::MessageView& message) {
    if (limit != 0 && seen >= limit) {
      return;
    }
    ++seen;

    const char type = message.raw_type();

    // System events carry the session boundaries and cost twelve bytes each.
    if (type == static_cast<char>(ob::itch::MessageType::system_event)) {
      keep(message);
      ++kept;
      return;
    }

    // Only the directory entry for the requested symbol is kept. Keeping all
    // 8500 of them would add roughly 350 KB of irrelevant fixture, and the
    // driver only needs the one it filters on.
    if (type == static_cast<char>(ob::itch::MessageType::stock_directory)) {
      if (message.symbol_at(ob::itch::stock_directory::STOCK) == symbol) {
        locate = message.stock_locate();
        resolved = true;
        keep(message);
        ++kept;
      }
      return;
    }

    if (resolved && message.stock_locate() == locate) {
      keep(message);
      ++kept;
    }
  }));

  if (!resolved) {
    std::cerr << "symbol " << symbol << " not found in the stock directory\n";
    std::exit(1);
  }

  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    std::cerr << "cannot open " << path << " for writing\n";
    std::exit(1);
  }

  // std::byte and char are both byte types, so viewing one as the other is
  // permitted rather than an aliasing violation, and ostream has no std::byte
  // overload.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  file.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
  if (!file.good()) {
    std::cerr << "write failed for " << path << '\n';
    std::exit(1);
  }

  std::cout << "extracted " << kept << " messages for " << symbol << " (locate " << locate
            << ") into " << path << ", " << out.size() << " bytes\n";
}

struct ReplayOptions {
  std::string symbol;
  std::uint32_t arena = 1U << 18;
  std::size_t cold = 8192;
  std::size_t limit = 0;
  std::int64_t tick_size = 100;
  std::string json_path;
  std::string depth_path;
  std::size_t depth_levels = 20;
  std::size_t depth_at = 0;
  bool depth_at_peak = false;
};

// One side of the ladder, captured at a chosen moment rather than read off the book
// at the end. A full trading day closes with everything deleted, so the end state
// is an empty book: correct, and useless as an illustration of book shape.
struct Ladder {
  std::vector<ReplayBook::LevelSnapshot> bids;
  std::vector<ReplayBook::LevelSnapshot> asks;
  std::size_t at_message = 0;
  std::size_t live_orders = 0;
  std::size_t occupied_bid_levels = 0;
  std::size_t occupied_ask_levels = 0;
  std::optional<ob::Ticks> bid;
  std::optional<ob::Ticks> ask;
  bool at_peak_depth = false;
};

// What the replay observed, separated from how it is reported. The text block and
// the JSON artifact are two renderings of this one struct, so they cannot come to
// disagree about a number the way two independent print paths would.
struct ReplayOutcome {
  std::size_t peak_live_orders = 0;
  std::size_t peak_live_orders_at_message = 0;
  std::size_t peak_occupied_levels = 0;
  std::int32_t lowest_tick = 0;
  std::int32_t highest_tick = 0;
  bool touch_seen = false;
  Ladder ladder;
};

// Binary rather than text, which is not an optimisation. On Windows a text mode
// stream turns every newline into a carriage return and a line feed, so the same
// artifact would be two different files depending on which host wrote it, and the
// CI guard that diffs bytes would fail on every developer's commit.
[[nodiscard]] std::ofstream open_output(const std::string& path) {
  std::ofstream file(path, std::ios::trunc | std::ios::binary);
  if (!file) {
    std::cerr << "cannot open " << path << " for writing\n";
    std::exit(1);
  }
  return file;
}

// Captures both sides of the ladder from the book as it stands right now.
// snapshot_levels returns a side ascending by price, so bids are reversed and asks
// are not: nearest the touch means the highest bid and the lowest ask.
void capture_ladder(Ladder& ladder, const ReplayBook& book, std::size_t at_message) {
  book.snapshot_levels(ob::Side::buy, ladder.bids);
  std::ranges::reverse(ladder.bids);
  book.snapshot_levels(ob::Side::sell, ladder.asks);

  ladder.at_message = at_message;
  ladder.live_orders = book.pool().live_count();
  ladder.occupied_bid_levels = book.occupied_level_count(ob::Side::buy);
  ladder.occupied_ask_levels = book.occupied_level_count(ob::Side::sell);
  ladder.bid = book.best_bid();
  ladder.ask = book.best_ask();
}

void write_side(ob::json::Writer& writer,
                const ReplayBook& book,
                const std::vector<ReplayBook::LevelSnapshot>& levels,
                std::size_t limit) {
  writer.begin_array();
  for (std::size_t index = 0; index < levels.size() && index < limit; ++index) {
    const ReplayBook::LevelSnapshot& level = levels[index];
    writer.begin_object();
    writer.field("tick", level.price.raw());
    writer.field("price", book.price_config().to_price(level.price).raw());
    writer.field("quantity", static_cast<std::uint64_t>(level.aggregate_qty.raw()));
    writer.field("orders", level.order_count);
    writer.end_object();
  }
  writer.end_array();
}

void write_touch(ob::json::Writer& writer,
                 const ReplayBook& book,
                 const char* tick_key,
                 const char* price_key,
                 const std::optional<ob::Ticks>& price) {
  if (!price.has_value()) {
    writer.null_field(tick_key);
    writer.null_field(price_key);
    return;
  }
  writer.field(tick_key, price->raw());
  writer.field(price_key, book.price_config().to_price(*price).raw());
}

void write_depth_json(const ReplayOptions& options,
                      const ob::json::Provenance& provenance,
                      const ReplayBook& book,
                      const Ladder& ladder) {
  std::ofstream file = open_output(options.depth_path);
  ob::json::Writer writer(file);

  writer.begin_object();
  ob::json::write_header(writer, provenance);
  writer.field("symbol", options.symbol);
  writer.field("levels_per_side", options.depth_levels);
  writer.field("price_scale", static_cast<std::int64_t>(ob::itch::PRICE_SCALE));
  writer.field("tick_size", options.tick_size);

  // When the snapshot was taken, and whether that was the deepest moment of the
  // replay. A consumer that assumed end of replay would draw an empty ladder for
  // any full trading day, and be right to.
  writer.field("captured_at_message", ladder.at_message);
  writer.field("captured_at_peak_depth", ladder.at_peak_depth);
  writer.field("live_orders_at_capture", ladder.live_orders);
  writer.field("occupied_bid_levels", ladder.occupied_bid_levels);
  writer.field("occupied_ask_levels", ladder.occupied_ask_levels);
  write_touch(writer, book, "bid_tick", "bid_price", ladder.bid);
  write_touch(writer, book, "ask_tick", "ask_price", ladder.ask);

  writer.key("bids");
  write_side(writer, book, ladder.bids, options.depth_levels);
  writer.key("asks");
  write_side(writer, book, ladder.asks, options.depth_levels);

  writer.end_object();
  writer.finish();
}

void write_replay_json(const ReplayOptions& options,
                       const ob::json::Provenance& provenance,
                       const ReplayBook& book,
                       const Driver& driver,
                       const ob::itch::ParseResult& result,
                       const ReplayOutcome& outcome) {
  const ob::itch::ReplayStats& stats = driver.stats();

  std::ofstream file = open_output(options.json_path);
  ob::json::Writer writer(file);

  writer.begin_object();
  ob::json::write_header(writer, provenance);

  writer.field("symbol", options.symbol);

  writer.key("config");
  writer.begin_object();
  writer.field("band_levels", static_cast<std::uint64_t>(REPLAY_BAND));
  writer.field("arena_capacity", options.arena);
  writer.field("max_cold_levels_per_side", options.cold);
  writer.field("tick_size", options.tick_size);
  writer.field("price_scale", static_cast<std::int64_t>(ob::itch::PRICE_SCALE));
  writer.field("message_limit", options.limit);
  writer.end_object();

  writer.key("parse");
  writer.begin_object();
  writer.field("status", static_cast<std::int64_t>(result.status));
  writer.field("messages", result.messages);
  writer.field("bytes_consumed", result.bytes_consumed);
  writer.field("unknown_types", result.unknown_types);
  writer.end_object();

  writer.key("locate");
  writer.begin_object();
  writer.field("resolved", driver.symbol_resolved());
  writer.field("code", driver.resolved_locate());
  writer.end_object();

  writer.key("messages");
  writer.begin_object();
  writer.field("applied", stats.messages_applied);
  writer.field("other_symbol", stats.other_symbol);
  writer.field("adds", stats.adds);
  writer.field("executions", stats.executions);
  writer.field("cancels", stats.cancels);
  writer.field("deletes", stats.deletes);
  writer.field("replaces", stats.replaces);
  writer.field("trades", stats.trades);
  writer.field("cross_trades", stats.cross_trades);
  writer.field("broken_trades", stats.broken_trades);
  writer.field("system_events", stats.system_events);
  writer.end_object();

  writer.key("rejections");
  writer.begin_object();
  writer.field("off_tick_prices", stats.off_tick_prices);
  writer.field("rejected_adds", stats.rejected_adds);
  writer.field("unknown_order_references", stats.unknown_order_references);
  writer.end_object();

  writer.key("book");
  writer.begin_object();
  writer.field("final_resting_orders", book.pool().live_count());
  writer.field("occupied_bid_levels", book.occupied_level_count(ob::Side::buy));
  writer.field("occupied_ask_levels", book.occupied_level_count(ob::Side::sell));
  writer.field("rebases", book.rebase_count());
  writer.field("rebases_abandoned", book.rebase_skipped_count());
  writer.field("cold_levels", book.cold_level_count());
  writer.field("cold_operations", book.cold_operation_count());
  writer.end_object();

  // The evidence behind the two capacity constants, in the artifact rather than
  // only in the printed block, because the site shows the peak against the arena
  // default and that comparison is the point of the whole section.
  writer.key("sizing");
  writer.begin_object();
  writer.field("peak_live_orders", outcome.peak_live_orders);
  // Where in the stream the peak happened, which is what --depth-at needs to
  // snapshot the ladder at the deepest moment rather than at an empty close.
  writer.field("peak_live_orders_at_message", outcome.peak_live_orders_at_message);
  writer.field("arena_capacity", book.pool().capacity());
  // The library's own default, which is the number the sizing story is about. This
  // tool runs a deliberately larger arena because it is pointed at arbitrary
  // symbols whose depth is unknown before the run, so its own capacity is the
  // wrong yardstick for "would the default have held".
  writer.field("library_arena_default", ReplayBook::Config{}.arena_capacity);
  writer.field("peak_occupied_levels", outcome.peak_occupied_levels);
  writer.field("band_levels", static_cast<std::uint64_t>(REPLAY_BAND));
  if (outcome.touch_seen) {
    const std::int64_t span = static_cast<std::int64_t>(outcome.highest_tick) - outcome.lowest_tick;
    writer.field("touch_low_tick", outcome.lowest_tick);
    writer.field("touch_high_tick", outcome.highest_tick);
    writer.field("touch_span_ticks", span);
    const double percent = static_cast<double>(span) * 100.0 / static_cast<double>(REPLAY_BAND);
    writer.field("touch_span_band_percent", percent, ob::json::PERCENT_DECIMALS);
  } else {
    writer.null_field("touch_low_tick");
    writer.null_field("touch_high_tick");
    writer.null_field("touch_span_ticks");
    writer.null_field("touch_span_band_percent");
  }
  writer.end_object();

  writer.key("final_touch");
  writer.begin_object();
  write_touch(writer, book, "bid_tick", "bid_price", book.best_bid());
  write_touch(writer, book, "ask_tick", "ask_price", book.best_ask());
  writer.end_object();

  writer.end_object();
  writer.finish();
}

void replay_symbol(std::span<const std::byte> data,
                   const ReplayOptions& options,
                   const ob::json::Provenance& provenance) {
  const std::string& symbol = options.symbol;
  const std::uint32_t arena = options.arena;
  const std::size_t cold = options.cold;
  const std::size_t limit = options.limit;
  const std::int64_t tick_size = options.tick_size;

  ReplayBook::Config config;
  // Base price zero and a penny tick, so a tick index is simply the price in
  // hundredths of a cent divided by 100. Real ITCH prices are absolute, so unlike
  // the synthetic tests there is no meaningful reference price to centre on.
  config.price = ob::PriceConfig{
      .tick_size = tick_size, .price_scale = ob::itch::PRICE_SCALE, .base_price = 0};
  config.arena_capacity = arena;
  config.max_cold_levels_per_side = cold;
  config.initial_center = ob::Ticks{0};

  const std::unique_ptr<ReplayBook> book = std::make_unique<ReplayBook>(config);
  Driver driver(*book, symbol);

  // Peaks and the traded price range, tracked so the band width and the arena
  // capacity can be justified from data rather than chosen and defended
  // afterwards. Both defaults were originally picked as plausible round numbers,
  // which is a weak position for a project whose whole argument is measurement.
  ReplayOutcome outcome;
  std::size_t peak_live_orders = 0;
  std::size_t peak_at_message = 0;
  std::size_t peak_occupied_levels = 0;
  std::int32_t lowest_tick = std::numeric_limits<std::int32_t>::max();
  std::int32_t highest_tick = std::numeric_limits<std::int32_t>::min();

  std::size_t seen = 0;
  const ob::itch::ParseResult result =
      ob::itch::for_each_message(data, [&](const ob::itch::MessageView& message) {
        if (limit != 0 && seen >= limit) {
          return;
        }
        ++seen;
        driver.apply(message);

        if (book->pool().live_count() > peak_live_orders) {
          peak_live_orders = book->pool().live_count();
          peak_at_message = seen;

          // Captured on every new record rather than found in a second pass, so
          // the ladder and the peak it belongs to come from one replay. The cost
          // is a bitmap walk per record, and records are rare once the book has
          // filled: a few thousand over a day of millions of messages.
          if (options.depth_at_peak) {
            capture_ladder(outcome.ladder, *book, seen);
          }
        }

        if (options.depth_at != 0 && seen == options.depth_at) {
          capture_ladder(outcome.ladder, *book, seen);
        }

        peak_occupied_levels = std::max<std::size_t>(
            peak_occupied_levels,
            book->occupied_level_count(ob::Side::buy) + book->occupied_level_count(ob::Side::sell));

        // The traded range is taken from the touch rather than from every resting
        // price, because a band has to cover where the market goes, and a lone
        // resting order far from the touch is what the cold path exists for.
        if (const std::optional<ob::Ticks> best_bid = book->best_bid(); best_bid.has_value()) {
          lowest_tick = std::min(lowest_tick, best_bid->raw());
          highest_tick = std::max(highest_tick, best_bid->raw());
        }
        if (const std::optional<ob::Ticks> best_ask = book->best_ask(); best_ask.has_value()) {
          lowest_tick = std::min(lowest_tick, best_ask->raw());
          highest_tick = std::max(highest_tick, best_ask->raw());
        }
      });

  const ob::itch::ReplayStats& stats = driver.stats();

  std::cout << "symbol                     " << symbol << '\n'
            << "locate resolved            " << (driver.symbol_resolved() ? "yes" : "no") << " ("
            << driver.resolved_locate() << ")\n"
            << "parse status               " << static_cast<int>(result.status) << '\n'
            << "messages parsed            " << result.messages << '\n'
            << "bytes consumed             " << result.bytes_consumed << " of " << data.size()
            << '\n'
            << "unknown message types      " << result.unknown_types << "\n\n"
            << "applied to this symbol     " << stats.messages_applied << '\n'
            << "filtered other symbols     " << stats.other_symbol << '\n'
            << "  adds                     " << stats.adds << '\n'
            << "  executions               " << stats.executions << '\n'
            << "  cancels                  " << stats.cancels << '\n'
            << "  deletes                  " << stats.deletes << '\n'
            << "  replaces                 " << stats.replaces << '\n'
            << "  trades (ignored)         " << stats.trades << '\n'
            << "  cross trades (ignored)   " << stats.cross_trades << '\n'
            << "  broken trades (ignored)  " << stats.broken_trades << "\n\n"
            << "off-tick prices            " << stats.off_tick_prices << '\n'
            << "rejected adds              " << stats.rejected_adds << '\n'
            << "unknown order references   " << stats.unknown_order_references << "\n\n"
            << "final resting orders       " << book->pool().live_count() << '\n'
            << "occupied bid levels        " << book->occupied_level_count(ob::Side::buy) << '\n'
            << "occupied ask levels        " << book->occupied_level_count(ob::Side::sell) << '\n'
            << "band rebases               " << book->rebase_count() << '\n'
            << "rebases abandoned          " << book->rebase_skipped_count() << '\n'
            << "cold levels                " << book->cold_level_count() << '\n'
            << "cold operations            " << book->cold_operation_count() << "\n\n";

  // The evidence for the two capacity constants, printed so they can be checked
  // against a real symbol rather than taken on trust.
  std::cout << "sizing evidence\n";
  std::cout << "  peak live orders         " << peak_live_orders << " against an arena of "
            << book->pool().capacity() << '\n';
  std::cout << "  peak occupied levels     " << peak_occupied_levels << " against a band of "
            << REPLAY_BAND << '\n';
  if (highest_tick >= lowest_tick) {
    const std::int64_t span = static_cast<std::int64_t>(highest_tick) - lowest_tick;
    std::cout << "  touch range              " << span << " ticks (" << lowest_tick << " to "
              << highest_tick << "), " << (static_cast<double>(span) * 100.0 / REPLAY_BAND)
              << " percent of the band\n";
  }

  const std::optional<ob::Ticks> bid = book->best_bid();
  const std::optional<ob::Ticks> ask = book->best_ask();
  std::cout << "final best bid             ";
  if (bid.has_value()) {
    std::cout << book->price_config().to_price(*bid).raw() << " (tick " << bid->raw() << ")\n";
  } else {
    std::cout << "none\n";
  }
  std::cout << "final best ask             ";
  if (ask.has_value()) {
    std::cout << book->price_config().to_price(*ask).raw() << " (tick " << ask->raw() << ")\n";
  } else {
    std::cout << "none\n";
  }

  outcome.peak_live_orders = peak_live_orders;
  outcome.peak_live_orders_at_message = peak_at_message;
  outcome.peak_occupied_levels = peak_occupied_levels;
  outcome.touch_seen = highest_tick >= lowest_tick;
  if (outcome.touch_seen) {
    outcome.lowest_tick = lowest_tick;
    outcome.highest_tick = highest_tick;
  }

  // A --depth-at beyond the end of the stream would otherwise leave an empty
  // ladder that looks like a genuinely empty book. Say so and fall back.
  const bool requested_snapshot = options.depth_at != 0 || options.depth_at_peak;
  if (requested_snapshot && outcome.ladder.at_message == 0) {
    std::cerr << "no depth snapshot was taken before the stream ended at " << seen
              << " messages, snapshotting the end of replay instead\n";
  }
  if (!requested_snapshot || outcome.ladder.at_message == 0) {
    capture_ladder(outcome.ladder, *book, seen);
  }
  outcome.ladder.at_peak_depth = outcome.ladder.at_message == outcome.peak_live_orders_at_message;

  std::cout << "\ndepth snapshot taken at message " << outcome.ladder.at_message << " with "
            << outcome.ladder.live_orders << " live orders\n";
  std::cout << "peak live orders reached at message " << outcome.peak_live_orders_at_message
            << '\n';

  if (!options.json_path.empty()) {
    write_replay_json(options, provenance, *book, driver, result, outcome);
    std::cout << "wrote " << options.json_path << '\n';
  }
  if (!options.depth_path.empty()) {
    write_depth_json(options, provenance, *book, outcome.ladder);
    std::cout << "wrote " << options.depth_path << '\n';
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string path;
  std::string symbol;
  std::string extract_to;
  bool do_survey = false;
  std::size_t top = 20;
  std::uint32_t arena = 1U << 18;
  std::size_t cold = 8192;
  std::size_t limit = 0;
  std::int64_t tick_size = 100;
  std::string json_path;
  std::string depth_path;
  std::size_t depth_levels = 20;
  std::size_t depth_at = 0;
  bool depth_at_peak = false;
  std::string sha256;
  std::string value;

  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];

    if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    }
    if (argument == "--survey") {
      do_survey = true;
      continue;
    }
    if (argument == "--depth-at-peak") {
      depth_at_peak = true;
      continue;
    }
    if (match_option(argument, "--symbol", index, argc, argv, value)) {
      symbol = value;
      continue;
    }
    if (match_option(argument, "--extract", index, argc, argv, value)) {
      extract_to = value;
      continue;
    }
    if (match_option(argument, "--json", index, argc, argv, value)) {
      json_path = value;
      continue;
    }
    if (match_option(argument, "--json-depth", index, argc, argv, value)) {
      depth_path = value;
      continue;
    }
    if (match_option(argument, "--sha256", index, argc, argv, value)) {
      sha256 = value;
      continue;
    }
    std::uint64_t unsigned_value = 0;
    if (match_option(argument, "--depth-levels", index, argc, argv, value)) {
      if (!parse_u64(value, unsigned_value) || unsigned_value == 0) {
        bad_value("--depth-levels", value);
      }
      depth_levels = static_cast<std::size_t>(unsigned_value);
      continue;
    }
    if (match_option(argument, "--depth-at", index, argc, argv, value)) {
      if (!parse_u64(value, unsigned_value)) {
        bad_value("--depth-at", value);
      }
      depth_at = static_cast<std::size_t>(unsigned_value);
      continue;
    }
    if (match_option(argument, "--top", index, argc, argv, value)) {
      if (!parse_u64(value, unsigned_value)) {
        bad_value("--top", value);
      }
      top = static_cast<std::size_t>(unsigned_value);
      continue;
    }
    if (match_option(argument, "--arena", index, argc, argv, value)) {
      if (!parse_u64(value, unsigned_value) || unsigned_value == 0) {
        bad_value("--arena", value);
      }
      arena = static_cast<std::uint32_t>(unsigned_value);
      continue;
    }
    if (match_option(argument, "--cold", index, argc, argv, value)) {
      if (!parse_u64(value, unsigned_value)) {
        bad_value("--cold", value);
      }
      cold = static_cast<std::size_t>(unsigned_value);
      continue;
    }
    if (match_option(argument, "--limit", index, argc, argv, value)) {
      if (!parse_u64(value, unsigned_value)) {
        bad_value("--limit", value);
      }
      limit = static_cast<std::size_t>(unsigned_value);
      continue;
    }
    if (match_option(argument, "--tick", index, argc, argv, value)) {
      std::int64_t signed_value = 0;
      if (!parse_i64(value, signed_value) || signed_value <= 0) {
        bad_value("--tick", value);
      }
      tick_size = signed_value;
      continue;
    }

    if (!argument.empty() && argument.front() == '-') {
      std::cerr << "unknown option: " << argument << '\n';
      print_usage();
      return 2;
    }
    path = argument;
  }

  if (path.empty() || (!do_survey && symbol.empty())) {
    print_usage();
    return 2;
  }

  ob::itch::MappedFile mapped;
  if (!mapped.open(path)) {
    std::cerr << mapped.error() << '\n';
    return 1;
  }

  std::cout << "file                       " << path << '\n'
            << "size                       " << mapped.size() << " bytes\n\n";

  if (do_survey) {
    survey(mapped.bytes(), top);
    return 0;
  }

  if (!extract_to.empty()) {
    extract(mapped.bytes(), symbol, extract_to, limit);
    return 0;
  }

  ReplayOptions options;
  options.symbol = symbol;
  options.arena = arena;
  options.cold = cold;
  options.limit = limit;
  options.tick_size = tick_size;
  options.json_path = json_path;
  options.depth_path = depth_path;
  options.depth_levels = depth_levels;
  options.depth_at = depth_at;
  options.depth_at_peak = depth_at_peak;

  ob::json::Provenance provenance;
  provenance.tool = "itch_replay";
  provenance.version = OB_VERSION_STRING;
  provenance.source_path = path;
  provenance.source_bytes = mapped.size();
  provenance.source_sha256 = sha256;
  provenance.command = ob::json::command_line(argc, argv);

  replay_symbol(mapped.bytes(), options, provenance);
  return 0;
}
