#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "itch/mapped_file.hpp"
#include "strategy/backtest.hpp"

// Runs the market making strategy over a replayed ITCH capture and reports P&L
// attribution, markouts, and the crossed book statistics.
//
// Also runs the parameter sweep, which is framed as sensitivity analysis and
// nothing more. See the note printed with the sweep output and ROADMAP.md: the
// best cell of a grid searched over one day of one symbol is a property of that
// day, and presenting it as a discovered edge would be worse than omitting the
// sweep entirely.

namespace {

using ob::strategy::Backtest;
using ob::strategy::BacktestConfig;
using ob::strategy::MarkoutSet;

struct Options {
  std::string path;
  std::string symbol = "QQQ";
  std::int32_t offset_ticks = 1;
  std::uint32_t size = 100;
  std::int64_t position_limit = 1000;
  double skew_ticks = 0.0;
  std::uint64_t latency_us = 0;
  bool sweep = false;
  bool latency_sweep = false;
  bool help = false;
};

void print_usage() {
  std::cout << "ob_strategy_backtest, market making over a replayed ITCH capture\n\n"
            << "  --file <path>     ITCH 5.0 capture, required\n"
            << "  --symbol <sym>    symbol to trade, default QQQ\n"
            << "  --offset <ticks>  quote distance from mid, default 1\n"
            << "  --size <shares>   quote size per side, default 100\n"
            << "  --limit <shares>  position limit, default 1000\n"
            << "  --skew <ticks>    quote shift at full inventory, default 0\n"
            << "  --latency-us <n>  reaction latency in microseconds, default 0.\n"
            << "                    Zero is an idealised bound, not a realistic setting\n"
            << "  --sweep           run the parameter sensitivity grid\n"
            << "  --latency-sweep   run the latency sensitivity grid\n"
            << "  --help\n";
}

[[nodiscard]] bool parse_long(const std::string& text, long& out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);
  if (end == text.c_str() || end == nullptr || *end != '\0') {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse_double(const std::string& text, double& out) {
  if (text.empty()) {
    return false;
  }
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() || end == nullptr || *end != '\0') {
    return false;
  }
  out = value;
  return true;
}

[[nodiscard]] bool parse_options(int argc, char** argv, Options& options) {
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument = argv[index];

    const auto next_value = [&](std::string& out) {
      if (index + 1 >= argc) {
        return false;
      }
      out = argv[++index];
      return true;
    };

    if (argument == "--help" || argument == "-h") {
      options.help = true;
      return true;
    }
    if (argument == "--sweep") {
      options.sweep = true;
      continue;
    }
    if (argument == "--latency-sweep") {
      options.latency_sweep = true;
      continue;
    }
    if (argument == "--file") {
      if (!next_value(options.path)) {
        return false;
      }
      continue;
    }
    if (argument == "--symbol") {
      if (!next_value(options.symbol)) {
        return false;
      }
      continue;
    }

    std::string value;
    long parsed = 0;
    if (argument == "--offset") {
      if (!next_value(value) || !parse_long(value, parsed)) {
        return false;
      }
      options.offset_ticks = static_cast<std::int32_t>(parsed);
      continue;
    }
    if (argument == "--size") {
      if (!next_value(value) || !parse_long(value, parsed) || parsed < 1) {
        return false;
      }
      options.size = static_cast<std::uint32_t>(parsed);
      continue;
    }
    if (argument == "--limit") {
      if (!next_value(value) || !parse_long(value, parsed) || parsed < 1) {
        return false;
      }
      options.position_limit = parsed;
      continue;
    }
    if (argument == "--latency-us") {
      if (!next_value(value) || !parse_long(value, parsed) || parsed < 0) {
        return false;
      }
      options.latency_us = static_cast<std::uint64_t>(parsed);
      continue;
    }
    if (argument == "--skew") {
      double skew = 0.0;
      if (!next_value(value) || !parse_double(value, skew)) {
        return false;
      }
      options.skew_ticks = skew;
      continue;
    }

    std::cerr << "unknown argument: " << argument << "\n";
    return false;
  }
  return true;
}

BacktestConfig config_from(const Options& options) {
  BacktestConfig config;
  config.symbol = options.symbol;
  config.maker.quote_offset_ticks = options.offset_ticks;
  config.maker.quote_size = options.size;
  config.maker.position_limit = options.position_limit;
  config.maker.inventory_skew_ticks = options.skew_ticks;
  config.latency_ns = options.latency_us * 1000ULL;
  return config;
}

void print_markout(const char* label, const MarkoutSet& set) {
  std::cout << "  " << std::left << std::setw(8) << label << std::right << std::fixed
            << std::setprecision(4) << std::setw(12) << set.buy.per_share() << std::setw(12)
            << set.sell.per_share() << std::setw(12) << set.all.per_share() << std::setw(10)
            << set.all.fills << "\n"
            << std::defaultfloat;
}

void report(const Backtest<>& backtest) {
  const auto& pnl = backtest.pnl();
  const auto& crossed = backtest.crossed();

  std::cout << "\nP&L attribution, in ticks times shares\n";
  std::cout << std::fixed << std::setprecision(2);
  std::cout << "  Spread capture    " << std::setw(14) << pnl.spread_pnl() << "\n";
  std::cout << "  Inventory         " << std::setw(14) << pnl.inventory_pnl() << "\n";
  std::cout << "  Fees and rebates  " << std::setw(14) << pnl.fee_pnl() << "\n";
  std::cout << "  Total             " << std::setw(14) << pnl.total_pnl() << "\n";
  std::cout << std::defaultfloat;

  std::cout << "\nRisk and activity\n";
  std::cout << "  Final position    " << pnl.position() << "\n";
  std::cout << "  Max long/short    " << pnl.max_long() << " / " << pnl.max_short() << "\n";
  std::cout << "  Time holding      " << (pnl.time_holding_ns() / 1000000ULL) << " ms\n";
  std::cout << "  Max drawdown      " << std::fixed << std::setprecision(2) << pnl.max_drawdown()
            << std::defaultfloat << "\n";
  std::cout << "  Fills             " << pnl.fill_count() << " (" << pnl.filled_shares()
            << " shares)\n";
  std::cout << "  of which through  " << pnl.trade_through_fills()
            << ", inferred from the market trading past our price\n";
  std::cout << "  Quotes placed     " << backtest.quotes_placed() << ", cancelled "
            << backtest.quotes_cancelled() << ", post-only rejected " << backtest.quotes_rejected()
            << "\n";
  std::cout << "  Fill ratio        " << std::fixed << std::setprecision(4) << pnl.fill_ratio()
            << std::defaultfloat << " of quoted shares\n";
  std::cout << "  Sharpe            " << std::fixed << std::setprecision(3) << pnl.sharpe()
            << std::defaultfloat << " on " << pnl.sample_count()
            << " one second samples, not annualised\n";
  std::cout << "                    " << pnl.informative_sample_count() << " non-zero, from "
            << pnl.fill_count() << " fills\n";
  if (!pnl.sharpe_clears_minimum_events()) {
    std::cout << "                    NOT MEANINGFUL: fewer than 30 fills, so a mean and a\n"
              << "                    standard deviation do not describe anything here.\n";
  }
  std::cout << "                    Treat as descriptive regardless. The series is dominated by\n"
            << "                    mark to market on a position held for a long time, so the\n"
            << "                    samples are strongly autocorrelated, which understates the\n"
            << "                    variance and therefore overstates the ratio.\n";

  std::cout << "\nMarkouts, signed P&L per share against mid at each horizon\n";
  std::cout << "  " << std::left << std::setw(8) << "horizon" << std::right << std::setw(12)
            << "buy" << std::setw(12) << "sell" << std::setw(12) << "all" << std::setw(10)
            << "fills" << "\n";
  for (std::size_t index = 0; index < ob::strategy::MARKOUT_COUNT; ++index) {
    print_markout(ob::strategy::MARKOUT_LABELS[index], pnl.markout(index));
  }
  std::cout << "  Unresolved at end of replay: " << pnl.unresolved_markouts()
            << ". A horizon whose fills are mostly unresolved has not been measured.\n";
  std::cout << "  Persistently negative markouts mean the strategy is being adversely\n";
  std::cout << "  selected: the market moves against every fill shortly after it happens.\n";
  std::cout << "\n  Read these against the holding time above, not on their own. The longest\n";
  std::cout << "  horizon here is 300 s. If the strategy held inventory for hours, healthy\n";
  std::cout << "  markouts say each fill was individually fine and say nothing about the\n";
  std::cout << "  position built out of them, which is where the money actually went.\n";

  std::cout << "\nCrossed book, the honesty check\n";
  std::cout << "  Crossed intervals " << crossed.intervals << "\n";
  std::cout << "  Crossed time      " << (crossed.total_ns / 1000000ULL) << " ms\n";
  std::cout << "  Observations      " << std::fixed << std::setprecision(4)
            << (crossed.crossed_fraction() * 100.0) << " % of book updates" << std::defaultfloat
            << "\n";
  std::cout << "  Worst depth       " << crossed.worst_depth_ticks << " ticks\n";
  std::cout << "  A crossed interval is time the quote sat inside the real spread with\n";
  std::cout << "  nobody trading against it. See DESIGN.md for why those are not fills.\n";
}

// How much the zero latency assumption was worth, measured rather than argued.
//
// Everything else in this file varies a strategy parameter. This varies an
// assumption about the world, which is the more important axis: a parameter the
// strategy chooses can be tuned, while latency is imposed on it. The values span
// a co-located participant at a few microseconds through to something well off
// the critical path at a millisecond.
void run_latency_sweep(std::span<const std::byte> data, const Options& options) {
  const std::vector<std::uint64_t> latencies_us{0, 1, 5, 25, 100, 500, 1000};

  std::cout << "\nlatency_us,total_pnl,spread,inventory,fills,filled_shares,fill_ratio,"
            << "quotes_placed,markout_1s_per_share,crossed_pct\n";

  for (const std::uint64_t latency : latencies_us) {
    Options local = options;
    local.latency_us = latency;

    Backtest<> backtest(config_from(local));
    backtest.run(data);

    const auto& pnl = backtest.pnl();
    std::cout << latency << "," << std::fixed << std::setprecision(2) << pnl.total_pnl() << ","
              << pnl.spread_pnl() << "," << pnl.inventory_pnl() << "," << pnl.fill_count() << ","
              << pnl.filled_shares() << "," << std::setprecision(4) << pnl.fill_ratio() << ","
              << backtest.quotes_placed() << "," << pnl.markout_1s().all.per_share() << ","
              << (backtest.crossed().crossed_fraction() * 100.0) << std::defaultfloat << "\n";
  }

  std::cout << "\nZero is the idealised bound and is not a realistic setting for any real\n"
            << "participant.\n\n"
            << "Read the fill count and the markout together, because separately they say\n"
            << "opposite things. Fills rise as latency grows, which looks like latency\n"
            << "helping and is not: the cancel is delayed as well as the placement, so a\n"
            << "quote the strategy has already decided to pull stays resting and fillable\n"
            << "for the whole window. More exposure, more fills.\n\n"
            << "The 1 s markout is what those extra fills are worth, and it falls as latency\n"
            << "rises. That is adverse selection appearing exactly where theory says it\n"
            << "should: the trades you cannot pull away from are disproportionately the ones\n"
            << "you would most have wanted to.\n";
}

void run_sweep(std::span<const std::byte> data, const Options& options) {
  const std::vector<std::int32_t> offsets{1, 2, 4};
  const std::vector<double> skews{0.0, 1.0, 2.0};
  const std::vector<std::uint32_t> sizes{100, 300};

  std::cout << "\noffset_ticks,skew_ticks,size,total_pnl,spread,inventory,fees,fills,"
            << "fill_ratio,max_drawdown,markout_1s_per_share,crossed_pct\n";

  for (const std::int32_t offset : offsets) {
    for (const double skew : skews) {
      for (const std::uint32_t size : sizes) {
        Options local = options;
        local.offset_ticks = offset;
        local.skew_ticks = skew;
        local.size = size;

        Backtest<> backtest(config_from(local));
        backtest.run(data);

        const auto& pnl = backtest.pnl();
        std::cout << offset << "," << skew << "," << size << "," << std::fixed
                  << std::setprecision(2) << pnl.total_pnl() << "," << pnl.spread_pnl() << ","
                  << pnl.inventory_pnl() << "," << pnl.fee_pnl() << "," << pnl.fill_count() << ","
                  << std::setprecision(4) << pnl.fill_ratio() << "," << std::setprecision(2)
                  << pnl.max_drawdown() << "," << std::setprecision(4)
                  << pnl.markout_1s().all.per_share() << ","
                  << (backtest.crossed().crossed_fraction() * 100.0) << std::defaultfloat << "\n";
      }
    }
  }

  std::cout << "\nThis is sensitivity analysis, not a search for an edge. The grid covers one\n"
            << "symbol on one day, so the best cell is a property of that day and nothing\n"
            << "more. What is worth reading is the shape: whether P&L degrades smoothly as\n"
            << "the quote widens, whether skew reduces drawdown, and whether the markout\n"
            << "stays negative everywhere, which would say the fills are adversely selected\n"
            << "regardless of parameters.\n";
}

}  // namespace

int main(int argc, char** argv) {
  Options options;
  if (!parse_options(argc, argv, options)) {
    print_usage();
    return 2;
  }
  if (options.help) {
    print_usage();
    return 0;
  }
  if (options.path.empty()) {
    std::cerr << "--file is required\n\n";
    print_usage();
    return 2;
  }

  ob::itch::MappedFile file;
  if (!file.open(options.path)) {
    std::cerr << "cannot open " << options.path << ": " << file.error() << "\n";
    return 1;
  }

  std::cout << "Backtest\n";
  std::cout << "  File              " << options.path << "\n";
  std::cout << "  Symbol            " << options.symbol << "\n";

  if (options.latency_sweep) {
    run_latency_sweep(file.bytes(), options);
    return 0;
  }

  if (options.sweep) {
    run_sweep(file.bytes(), options);
    return 0;
  }

  std::cout << "  Quote offset      " << options.offset_ticks << " ticks\n";
  std::cout << "  Quote size        " << options.size << " shares\n";
  std::cout << "  Position limit    " << options.position_limit << " shares\n";
  std::cout << "  Inventory skew    " << options.skew_ticks << " ticks at full inventory\n";
  std::cout << "  Reaction latency  " << options.latency_us << " us";
  if (options.latency_us == 0) {
    std::cout << ", an idealised bound rather than a realistic setting";
  }
  std::cout << "\n";

  Backtest<> backtest(config_from(options));
  backtest.run(file.bytes());

  const auto& stats = backtest.replay_stats();
  std::cout << "  Messages applied  " << stats.messages_applied << " of " << stats.messages_seen
            << "\n";

  report(backtest);
  return 0;
}
