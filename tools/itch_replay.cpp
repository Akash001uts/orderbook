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

--extract is how a multi-gigabyte capture becomes a test fixture small enough to
commit: it keeps the system event and stock directory messages the replay driver
needs, plus every message for the chosen symbol, and drops the rest.
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

void replay_symbol(std::span<const std::byte> data,
                   const std::string& symbol,
                   std::uint32_t arena,
                   std::size_t cold,
                   std::size_t limit,
                   std::int64_t tick_size) {
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
  std::size_t peak_live_orders = 0;
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

        peak_live_orders = std::max<std::size_t>(peak_live_orders, book->pool().live_count());
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
    if (match_option(argument, "--symbol", index, argc, argv, value)) {
      symbol = value;
      continue;
    }
    if (match_option(argument, "--extract", index, argc, argv, value)) {
      extract_to = value;
      continue;
    }
    if (match_option(argument, "--top", index, argc, argv, value)) {
      top = static_cast<std::size_t>(std::stoull(value));
      continue;
    }
    if (match_option(argument, "--arena", index, argc, argv, value)) {
      arena = static_cast<std::uint32_t>(std::stoul(value));
      continue;
    }
    if (match_option(argument, "--cold", index, argc, argv, value)) {
      cold = static_cast<std::size_t>(std::stoull(value));
      continue;
    }
    if (match_option(argument, "--limit", index, argc, argv, value)) {
      limit = static_cast<std::size_t>(std::stoull(value));
      continue;
    }
    if (match_option(argument, "--tick", index, argc, argv, value)) {
      tick_size = std::stoll(value);
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

  replay_symbol(mapped.bytes(), symbol, arena, cold, limit, tick_size);
  return 0;
}
